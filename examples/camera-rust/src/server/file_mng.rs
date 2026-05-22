use anyhow::Result;
use std::collections::BTreeMap;
use std::fs::{File, OpenOptions};
use std::io::{Read, Seek, SeekFrom, Write};
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};
use std::thread;
use tracing::{info, warn, error};

use super::client_connect::{ClientConnect, ClientConnectListener};

const FILE_MNG_PORT: u16 = 56060;
const MAX_FILE_COUNT: usize = 10;
const MAX_FILE_SIZE: u64 = 1024 * 1024 * 1024;
const MOOV_HEAD_LEN: usize = 100 * std::mem::size_of::<i64>();

pub trait FileMngListener: Send + Sync {
    fn on_new_file_create(&self);
}

struct FileMngListenerImpl {
    file_map: Arc<Mutex<BTreeMap<PathBuf, Vec<i64>>>>,
    idr_positions: Arc<Mutex<Vec<i64>>>,
    current_file_size: Arc<Mutex<u64>>,
}

impl ClientConnectListener for FileMngListenerImpl {
    fn get_file_map_clone(&self) -> BTreeMap<PathBuf, Vec<i64>> {
        self.file_map.lock().unwrap().clone()
    }
    
    fn get_last_file_size(&self) -> u64 {
        *self.current_file_size.lock().unwrap()
    }
    
    fn get_tmp_idr_pos_100(&self) -> Vec<i64> {
        let idr_pos = self.idr_positions.lock().unwrap();
        let mut result = vec![0i64; 100];
        let len = idr_pos.len().min(100);
        result[..len].copy_from_slice(&idr_pos[..len]);
        result
    }
}

pub struct FileMng {
    listener: Option<Arc<dyn FileMngListener>>,
    output_dir: PathBuf,
    current_file: Arc<Mutex<Option<File>>>,
    current_file_path: Arc<Mutex<PathBuf>>,
    file_map: Arc<Mutex<BTreeMap<PathBuf, Vec<i64>>>>,
    idr_positions: Arc<Mutex<Vec<i64>>>,
    current_file_size: Arc<Mutex<u64>>,
    running: Arc<Mutex<bool>>,
    thread: Arc<Mutex<Option<thread::JoinHandle<()>>>>,
    server_fd: i32,
    connections: Arc<Mutex<BTreeMap<i32, Arc<Mutex<ClientConnect>>>>>,
}

impl FileMng {
    pub fn new(listener: Option<Arc<dyn FileMngListener>>) -> Result<Self> {
        let output_dir = PathBuf::from("./video");
        std::fs::create_dir_all(&output_dir)?;
        
        let file_map: Arc<Mutex<BTreeMap<PathBuf, Vec<i64>>>> = Arc::new(Mutex::new(BTreeMap::new()));
        let idr_positions: Arc<Mutex<Vec<i64>>> = Arc::new(Mutex::new(Vec::new()));
        let current_file_size: Arc<Mutex<u64>> = Arc::new(Mutex::new(0));
        let running = Arc::new(Mutex::new(true));
        let connections: Arc<Mutex<BTreeMap<i32, Arc<Mutex<ClientConnect>>>>> = Arc::new(Mutex::new(BTreeMap::new()));
        
        let mut instance = Self {
            listener,
            output_dir,
            current_file: Arc::new(Mutex::new(None)),
            current_file_path: Arc::new(Mutex::new(PathBuf::new())),
            file_map: file_map.clone(),
            idr_positions: idr_positions.clone(),
            current_file_size: current_file_size.clone(),
            running: running.clone(),
            thread: Arc::new(Mutex::new(None)),
            server_fd: -1,
            connections: connections.clone(),
        };
        
        instance.load_existing_files()?;
        instance.create_new_file()?;
        
        let running_clone = running.clone();
        let connections_clone = connections.clone();
        let file_map_clone = file_map.clone();
        let idr_positions_clone = idr_positions.clone();
        let current_file_size_clone = current_file_size.clone();
        
        let thread_handle = thread::spawn(move || {
            Self::accept_loop(
                running_clone,
                connections_clone,
                file_map_clone,
                idr_positions_clone,
                current_file_size_clone,
            );
        });
        
        *instance.thread.lock().unwrap() = Some(thread_handle);
        
        info!("FileMng initialized, playback server on port {}", FILE_MNG_PORT);
        
        Ok(instance)
    }
    
    fn load_existing_files(&mut self) -> Result<()> {
        let mut file_map = self.file_map.lock().unwrap();
        
        if self.output_dir.exists() {
            for entry in std::fs::read_dir(&self.output_dir)? {
                let entry = entry?;
                let path = entry.path();
                
                if path.extension().map_or(false, |ext| ext == "h264") {
                    if let Ok(mut file) = File::open(&path) {
                        let mut idr_pos = vec![0i64; 100];
                        if file.read_exact(unsafe {
                            std::slice::from_raw_parts_mut(
                                idr_pos.as_mut_ptr() as *mut u8,
                                MOOV_HEAD_LEN
                            )
                        }).is_ok() {
                            file_map.insert(path, idr_pos);
                        }
                    }
                }
            }
        }
        
        while file_map.len() > MAX_FILE_COUNT {
            if let Some((oldest_path, _)) = file_map.iter().next().map(|(k, v)| (k.clone(), v.clone())) {
                let _ = std::fs::remove_file(&oldest_path);
                file_map.remove(&oldest_path);
                info!("Removed old file: {:?}", oldest_path);
            }
        }
        
        Ok(())
    }
    
    /// 均匀抽取100个I帧位置
    fn uniform_resize_to_100(&self, idr_positions: &[i64]) -> Vec<i64> {
        let mut result = vec![0i64; 100];
        
        if idr_positions.is_empty() {
            return result;
        }
        
        if idr_positions.len() <= 100 {
            // 如果I帧数量少于100，直接复制
            result[..idr_positions.len()].copy_from_slice(idr_positions);
        } else {
            // 均匀抽取100个
            let step = idr_positions.len() as f64 / 100.0;
            for i in 0..100 {
                let index = (i as f64 * step) as usize;
                let index = index.min(idr_positions.len() - 1);
                result[i] = idr_positions[index];
            }
        }
        
        result
    }
    
    /// 关闭当前文件并写入I帧位置信息
    fn close_current_file(&self) -> Result<()> {
        let mut current_file = self.current_file.lock().unwrap();
        if let Some(mut file) = current_file.take() {
            let idr_positions = self.idr_positions.lock().unwrap();
            
            // 均匀抽取100个I帧位置
            let idr_pos_100 = self.uniform_resize_to_100(&idr_positions);
            
            // 写入文件头
            file.seek(SeekFrom::Start(0))?;
            file.write_all(unsafe {
                std::slice::from_raw_parts(
                    idr_pos_100.as_ptr() as *const u8,
                    MOOV_HEAD_LEN
                )
            })?;
            file.flush()?;
            
            // 更新file_map中的I帧位置信息
            let current_file_path = self.current_file_path.lock().unwrap();
            let mut file_map = self.file_map.lock().unwrap();
            if let Some(pos) = file_map.get_mut(&*current_file_path) {
                *pos = idr_pos_100.clone();
            }
            
            info!("Closed file: {:?}, wrote {} I-frame positions", 
                  *current_file_path, idr_positions.len());
        }
        
        Ok(())
    }
    
    fn create_new_file(&self) -> Result<()> {
        // 先关闭当前文件（如果有）
        let _ = self.close_current_file();
        
        let timestamp = chrono::Local::now().format("%Y%m%d_%H%M%S").to_string();
        let filename = format!("{}.h264", timestamp);
        let path = self.output_dir.join(&filename);
        
        let mut file = OpenOptions::new()
            .write(true)
            .create(true)
            .truncate(true)
            .open(&path)?;
        
        let mut idr_pos = vec![0i64; 100];
        file.write_all(unsafe {
            std::slice::from_raw_parts(
                idr_pos.as_ptr() as *const u8,
                MOOV_HEAD_LEN
            )
        })?;
        
        *self.current_file.lock().unwrap() = Some(file);
        *self.current_file_path.lock().unwrap() = path.clone();
        *self.current_file_size.lock().unwrap() = MOOV_HEAD_LEN as u64;
        
        {
            let mut idr_positions = self.idr_positions.lock().unwrap();
            idr_positions.clear();
        }
        
        {
            let mut file_map = self.file_map.lock().unwrap();
            file_map.insert(path.clone(), idr_pos);
            
            while file_map.len() > MAX_FILE_COUNT {
                if let Some((oldest_path, _)) = file_map.iter().next().map(|(k, v)| (k.clone(), v.clone())) {
                    let _ = std::fs::remove_file(&oldest_path);
                    file_map.remove(&oldest_path);
                    info!("Removed old file: {:?}", oldest_path);
                }
            }
        }
        
        if let Some(ref listener) = self.listener {
            listener.on_new_file_create();
        }
        
        info!("Created new file: {:?}", path);
        
        Ok(())
    }
    
    pub fn input_file_data(&self, data: &[u8], flag: u8) {
        let mut current_file = self.current_file.lock().unwrap();
        if let Some(ref mut file) = *current_file {
            // 文件格式：4字节长度 + 1字节标志 + 数据（与原camera项目对齐）
            let data_len = data.len() + 1; // +1 for flag byte
            let len_bytes = (data_len as u32).to_le_bytes();
            
            if file.write_all(&len_bytes).is_ok() && 
               file.write_all(&[flag]).is_ok() && 
               file.write_all(data).is_ok() {
                let mut file_size = self.current_file_size.lock().unwrap();
                *file_size += 4 + 1 + data.len() as u64; // 4字节长度 + 1字节标志 + 数据
                
                // flag=1表示视频数据，检测I帧
                if flag == 1 {
                    if let Some(nal_type) = data.get(4) {
                        // NAL type 5 = IDR帧
                        if *nal_type & 0x1F == 5 {
                            let pos = *file_size - data.len() as u64 - 1 - 4; // 减去数据长度、标志位、长度字段
                            let mut idr_positions = self.idr_positions.lock().unwrap();
                            idr_positions.push(pos as i64);
                            
                            if idr_positions.len() <= 100 {
                                let current_file_path = self.current_file_path.lock().unwrap();
                                let mut file_map = self.file_map.lock().unwrap();
                                if let Some(idr_pos) = file_map.get_mut(&*current_file_path) {
                                    idr_pos[idr_positions.len() - 1] = pos as i64;
                                }
                            }
                        }
                    }
                }
                
                if *file_size >= MAX_FILE_SIZE {
                    drop(file_size);
                    drop(current_file);
                    let _ = self.create_new_file();
                }
            }
        }
    }
    
    fn accept_loop(
        running: Arc<Mutex<bool>>,
        connections: Arc<Mutex<BTreeMap<i32, Arc<Mutex<ClientConnect>>>>>,
        file_map: Arc<Mutex<BTreeMap<PathBuf, Vec<i64>>>>,
        idr_positions: Arc<Mutex<Vec<i64>>>,
        current_file_size: Arc<Mutex<u64>>,
    ) {
        info!("FileMng accept loop started on port {}", FILE_MNG_PORT);
        
        unsafe {
            let server_fd = libc::socket(libc::AF_INET, libc::SOCK_STREAM, 0);
            if server_fd < 0 {
                error!("Failed to create socket");
                return;
            }
            
            let opt: i32 = 1;
            libc::setsockopt(
                server_fd,
                libc::SOL_SOCKET,
                libc::SO_REUSEADDR,
                &opt as *const i32 as *const libc::c_void,
                std::mem::size_of::<i32>() as u32,
            );
            
            let mut addr: libc::sockaddr_in = std::mem::zeroed();
            addr.sin_family = libc::AF_INET as u16;
            addr.sin_port = FILE_MNG_PORT.to_be();
            addr.sin_addr.s_addr = libc::INADDR_ANY;
            
            if libc::bind(
                server_fd,
                &addr as *const libc::sockaddr_in as *const libc::sockaddr,
                std::mem::size_of::<libc::sockaddr_in>() as u32,
            ) < 0 {
                error!("Failed to bind socket");
                libc::close(server_fd);
                return;
            }
            
            if libc::listen(server_fd, 10) < 0 {
                error!("Failed to listen on socket");
                libc::close(server_fd);
                return;
            }
            
            let mut fds: libc::fd_set = std::mem::zeroed();
            let mut timeout = libc::timeval {
                tv_sec: 1,
                tv_usec: 0,
            };
            
            while *running.lock().unwrap() {
                libc::FD_ZERO(&mut fds);
                libc::FD_SET(server_fd, &mut fds);
                
                let mut tmp_fds = fds;
                let tmp_timeout = timeout;
                
                let ret = libc::select(
                    server_fd + 1,
                    &mut tmp_fds,
                    std::ptr::null_mut(),
                    std::ptr::null_mut(),
                    &tmp_timeout as *const libc::timeval as *mut libc::timeval,
                );
                
                if ret < 0 {
                    if *running.lock().unwrap() {
                        error!("Select error");
                    }
                    break;
                }
                
                if ret == 0 {
                    continue;
                }
                
                if libc::FD_ISSET(server_fd, &tmp_fds) {
                    let mut client_addr: libc::sockaddr_in = std::mem::zeroed();
                    let mut client_addr_len = std::mem::size_of::<libc::sockaddr_in>() as u32;
                    
                    let client_fd = libc::accept(
                        server_fd,
                        &mut client_addr as *mut libc::sockaddr_in as *mut libc::sockaddr,
                        &mut client_addr_len,
                    );
                    
                    if client_fd >= 0 {
                        info!("New playback client connected: {}", client_fd);
                        
                        let listener = Arc::new(FileMngListenerImpl {
                            file_map: file_map.clone(),
                            idr_positions: idr_positions.clone(),
                            current_file_size: current_file_size.clone(),
                        });
                        
                        match ClientConnect::new(client_fd, listener) {
                            Ok(client) => {
                                let mut conns = connections.lock().unwrap();
                                conns.insert(client_fd, Arc::new(Mutex::new(client)));
                            }
                            Err(e) => {
                                error!("Failed to create ClientConnect: {:?}", e);
                                libc::close(client_fd);
                            }
                        }
                    }
                }
            }
            
            {
                let mut conns = connections.lock().unwrap();
                for (_, client) in conns.iter() {
                    let mut client = client.lock().unwrap();
                    client.stop();
                }
                conns.clear();
            }
            
            libc::close(server_fd);
        }
        
        info!("FileMng accept loop stopped");
    }
    
    pub fn stop(&self) {
        *self.running.lock().unwrap() = false;
        
        // 关闭当前文件并写入I帧位置信息
        let _ = self.close_current_file();
        
        {
            let mut conns = self.connections.lock().unwrap();
            for (_, client) in conns.iter() {
                let mut client = client.lock().unwrap();
                client.stop();
            }
            conns.clear();
        }
        
        if let Some(thread) = self.thread.lock().unwrap().take() {
            let _ = thread.join();
        }
        
        info!("FileMng stopped");
    }
}

impl Drop for FileMng {
    fn drop(&mut self) {
        self.stop();
    }
}
