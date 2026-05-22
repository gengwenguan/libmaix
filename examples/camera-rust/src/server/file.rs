use anyhow::Result;
use std::collections::BTreeMap;
use std::fs::{File, OpenOptions};
use std::io::{Read, Seek, SeekFrom, Write};
use std::path::{Path, PathBuf};
use std::sync::Arc;
use tokio::sync::Mutex;
use tracing::{info, warn};

use crate::nal::NALParser;

const FILE_DIR: &str = "video/";
const MAX_FILE_NUM: usize = 10;
const MAX_FILE_SIZE: u64 = 1024 * 1024 * 1024; // 1GB
const MOOV_HEAD_LEN: usize = 100 * std::mem::size_of::<i64>(); // 100个I帧位置

/// 文件管理器，负责保存视频文件并管理文件数量
pub struct FileManager {
    output_dir: PathBuf,
    current_file: Option<File>,
    current_file_path: PathBuf,
    file_map: BTreeMap<PathBuf, Vec<i64>>, // 文件路径 -> I帧位置列表
    idr_positions: Vec<i64>,               // 当前文件的所有I帧位置
    current_file_size: u64,
}

impl FileManager {
    pub fn new(output_dir: &str) -> Result<Self> {
        let output_dir = PathBuf::from(output_dir);
        
        // 创建输出目录
        std::fs::create_dir_all(&output_dir)?;
        
        let mut manager = Self {
            output_dir,
            current_file: None,
            current_file_path: PathBuf::new(),
            file_map: BTreeMap::new(),
            idr_positions: Vec::new(),
            current_file_size: 0,
        };
        
        // 扫描已有文件
        manager.scan_existing_files()?;
        
        // 创建新文件
        manager.create_new_file()?;
        
        Ok(manager)
    }
    
    /// 扫描目录中的已有文件
    fn scan_existing_files(&mut self) -> Result<()> {
        let entries = std::fs::read_dir(&self.output_dir)?;
        
        for entry in entries {
            let entry = entry?;
            let path = entry.path();
            
            if path.is_file() {
                let metadata = entry.metadata()?;
                let file_size = metadata.len();
                
                // 删除小于10KB的文件
                if file_size < 10240 {
                    warn!("Removing small file: {:?}", path);
                    std::fs::remove_file(&path)?;
                    continue;
                }
                
                // 读取I帧位置信息头
                let mut file = File::open(&path)?;
                let mut buffer = vec![0i64; 100];
                let bytes_read = file.read(unsafe {
                    std::slice::from_raw_parts_mut(
                        buffer.as_mut_ptr() as *mut u8,
                        MOOV_HEAD_LEN
                    )
                })?;
                
                if bytes_read == MOOV_HEAD_LEN {
                    // 检查是否包含有效的I帧位置信息
                    let has_valid_idr = buffer.iter().any(|&pos| pos != 0);
                    
                    if has_valid_idr {
                        self.file_map.insert(path, buffer);
                    } else {
                        warn!("Removing file without valid IDR info: {:?}", path);
                        std::fs::remove_file(&path)?;
                    }
                }
            }
        }
        
        info!("Scanned {} existing files", self.file_map.len());
        Ok(())
    }
    
    /// 创建新文件
    fn create_new_file(&mut self) -> Result<()> {
        // 生成基于当前时间的文件名
        let file_path = self.generate_file_path();
        info!("Creating new file: {:?}", file_path);
        
        // 创建文件并写入空的I帧位置头
        let mut file = OpenOptions::new()
            .write(true)
            .create(true)
            .truncate(true)
            .open(&file_path)?;
        
        // 写入100个0作为I帧位置头
        let header = vec![0i64; 100];
        file.write_all(unsafe {
            std::slice::from_raw_parts(
                header.as_ptr() as *const u8,
                MOOV_HEAD_LEN
            )
        })?;
        
        self.current_file_size = MOOV_HEAD_LEN as u64;
        self.idr_positions.clear();
        self.idr_positions.push(MOOV_HEAD_LEN as i64); // 第一个I帧位置
        
        self.current_file = Some(file);
        self.current_file_path = file_path.clone();
        self.file_map.insert(file_path, header);
        
        // 检查并删除最老的文件
        self.cleanup_old_files()?;
        
        Ok(())
    }
    
    /// 生成基于当前时间的文件路径
    fn generate_file_path(&self) -> PathBuf {
        let now = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap()
            .as_secs();
        
        let filename = format!("video_{}.h264", now);
        self.output_dir.join(filename)
    }
    
    /// 清理最老的文件
    fn cleanup_old_files(&mut self) -> Result<()> {
        // 当文件数量超过限制时，删除最老的文件
        while self.file_map.len() > MAX_FILE_NUM {
            if let Some((oldest_path, _)) = self.file_map.iter().next() {
                let path = oldest_path.clone();
                warn!("Removing oldest file: {:?}", path);
                std::fs::remove_file(&path)?;
                self.file_map.remove(&path);
            }
        }
        Ok(())
    }
    
    /// 写入数据到文件
    pub fn write_data(&mut self, data: &[u8], is_video: bool) -> Result<()> {
        if let Some(file) = &mut self.current_file {
            // 如果是视频数据，检查是否为I帧
            if is_video {
                let nal_type = NALParser::get_nal_type(data);
                if nal_type == crate::nal::NALUnitType::NAL_SPS 
                    || nal_type == crate::nal::NALUnitType::NAL_IDR {
                    // 记录I帧位置
                    self.idr_positions.push(self.current_file_size as i64);
                }
            }
            
            // 写入数据长度（4字节）
            let data_len = (data.len() + 1) as u32; // +1 for flag
            file.write_all(&data_len.to_le_bytes())?;
            
            // 写入标志位（1字节）
            let flag: u8 = if is_video { 1 } else { 0 };
            file.write_all(&[flag])?;
            
            // 写入实际数据
            file.write_all(data)?;
            
            self.current_file_size += 4 + 1 + data.len() as u64;
            
            // 检查文件大小是否达到限制
            if self.current_file_size >= MAX_FILE_SIZE {
                self.rotate_file()?;
            }
        }
        Ok(())
    }
    
    /// 切换到新文件
    fn rotate_file(&mut self) -> Result<()> {
        info!("Rotating file, current size: {}", self.current_file_size);
        
        // 关闭当前文件并写入I帧位置信息
        if let Some(mut file) = self.current_file.take() {
            // 均匀抽取100个I帧位置
            let idr_pos_100 = self.uniform_resize_to_100(&self.idr_positions);
            
            // 写入文件头
            file.seek(SeekFrom::Start(0))?;
            file.write_all(unsafe {
                std::slice::from_raw_parts(
                    idr_pos_100.as_ptr() as *const u8,
                    MOOV_HEAD_LEN
                )
            })?;
            
            // 更新文件map
            if let Some(entry) = self.file_map.get_mut(&self.current_file_path) {
                *entry = idr_pos_100;
            }
            
            file.flush()?;
        }
        
        // 创建新文件
        self.create_new_file()?;
        
        Ok(())
    }
    
    /// 将I帧位置列表均匀抽取为100个
    fn uniform_resize_to_100(&self, idr_pos: &[i64]) -> Vec<i64> {
        let mut result = vec![0i64; 100];
        
        if idr_pos.is_empty() {
            return result;
        }
        
        if idr_pos.len() <= 100 {
            // 如果少于100个，直接复制
            for (i, &pos) in idr_pos.iter().enumerate() {
                result[i] = pos;
            }
        } else {
            // 均匀抽取100个
            let step = idr_pos.len() as f64 / 100.0;
            for i in 0..100 {
                let index = (i as f64 * step) as usize;
                result[i] = idr_pos[index.min(idr_pos.len() - 1)];
            }
        }
        
        result
    }
    
    /// 获取文件列表
    pub fn get_file_list(&self) -> Vec<(PathBuf, Vec<i64>)> {
        self.file_map
            .iter()
            .map(|(path, idr_pos)| (path.clone(), idr_pos.clone()))
            .collect()
    }
    
    /// 停止文件管理器
    pub fn stop(&mut self) -> Result<()> {
        info!("Stopping file manager");
        
        // 关闭当前文件并写入I帧位置信息
        if let Some(mut file) = self.current_file.take() {
            let idr_pos_100 = self.uniform_resize_to_100(&self.idr_positions);
            
            file.seek(SeekFrom::Start(0))?;
            file.write_all(unsafe {
                std::slice::from_raw_parts(
                    idr_pos_100.as_ptr() as *const u8,
                    MOOV_HEAD_LEN
                )
            })?;
            
            if let Some(entry) = self.file_map.get_mut(&self.current_file_path) {
                *entry = idr_pos_100;
            }
            
            file.flush()?;
        }
        
        Ok(())
    }
}

impl Drop for FileManager {
    fn drop(&mut self) {
        let _ = self.stop();
    }
}
