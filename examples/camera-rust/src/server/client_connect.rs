use anyhow::Result;
use std::collections::BTreeMap;
use std::fs::File;
use std::io::{Read, Seek, SeekFrom};
use std::path::PathBuf;
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::Duration;
use tracing::{info, warn, error};

const TMP_BUFF_SIZE: usize = 4096;
const INTERVAL_DEFAULT_MS: u64 = 30;
const INTERVAL_FAST_PLAY_MS: u64 = 10;
const MOOV_HEAD_LEN: usize = 100 * std::mem::size_of::<i64>();

pub trait ClientConnectListener: Send + Sync {
    fn get_file_map_clone(&self) -> BTreeMap<PathBuf, Vec<i64>>;
    fn get_last_file_size(&self) -> u64;
    fn get_tmp_idr_pos_100(&self) -> Vec<i64>;
}

pub struct ClientConnect {
    socket_fd: i32,
    listener: Arc<dyn ClientConnectListener>,
    file_map_iter: Option<PathBuf>,
    send_file: Option<File>,
    idr_pos_100: Vec<i64>,
    progress: u8,
    running: Arc<Mutex<bool>>,
    thread: Option<thread::JoinHandle<()>>,
    message_list: Arc<Mutex<Vec<u8>>>,
    interval_ms: u64,
}

impl ClientConnect {
    pub fn new(socket_fd: i32, listener: Arc<dyn ClientConnectListener>) -> Result<Self> {
        info!("New ClientConnect for socket {}", socket_fd);
        
        let running = Arc::new(Mutex::new(true));
        let message_list = Arc::new(Mutex::new(Vec::new()));
        
        let mut client = Self {
            socket_fd,
            listener: listener.clone(),
            file_map_iter: None,
            send_file: None,
            idr_pos_100: vec![0i64; 100],
            progress: 0,
            running: running.clone(),
            thread: None,
            message_list: message_list.clone(),
            interval_ms: INTERVAL_DEFAULT_MS,
        };
        
        client.open_first_file()?;
        
        let running_clone = running.clone();
        let message_list_clone = message_list.clone();
        let socket_fd_clone = socket_fd;
        let listener_clone = listener.clone();
        let mut idr_pos_100_clone = client.idr_pos_100.clone();
        
        let thread = thread::spawn(move || {
            Self::send_file_task(
                socket_fd_clone,
                listener_clone,
                idr_pos_100_clone,
                running_clone,
                message_list_clone,
            );
        });
        
        client.thread = Some(thread);
        
        Ok(client)
    }
    
    fn open_first_file(&mut self) -> Result<()> {
        let file_map = self.listener.get_file_map_clone();
        
        if let Some((first_path, idr_pos)) = file_map.iter().next() {
            self.file_map_iter = Some(first_path.clone());
            self.idr_pos_100 = idr_pos.clone();
            
            let mut file = File::open(first_path)?;
            file.read_exact(unsafe {
                std::slice::from_raw_parts_mut(
                    self.idr_pos_100.as_mut_ptr() as *mut u8,
                    MOOV_HEAD_LEN
                )
            })?;
            
            if self.idr_pos_100.iter().all(|&pos| pos == 0) {
                let it = file_map.iter().next();
                if it.map(|(k, _)| k) == file_map.iter().last().map(|(k, _)| k) {
                    self.idr_pos_100 = self.listener.get_tmp_idr_pos_100();
                } else {
                    self.idr_pos_100 = idr_pos.clone();
                }
            }
            
            self.send_file = Some(file);
            info!("Opened first file: {:?}", first_path);
        }
        
        Ok(())
    }
    
    pub fn recv_ctrl_message(&mut self, data: &[u8]) -> Result<()> {
        if data.len() != 1 {
            warn!("Only support message length 1, got {}", data.len());
            return Ok(());
        }
        
        let message = data[0];
        info!("Received control message: {}", message);
        
        let mut list = self.message_list.lock().unwrap();
        list.push(message);
        
        Ok(())
    }
    
    fn send_file_task(
        socket_fd: i32,
        listener: Arc<dyn ClientConnectListener>,
        mut idr_pos_100: Vec<i64>,
        running: Arc<Mutex<bool>>,
        message_list: Arc<Mutex<Vec<u8>>>,
    ) {
        info!("Send file task started for socket {}", socket_fd);
        
        let mut progress: u8 = 0;
        let mut interval_ms = INTERVAL_DEFAULT_MS;
        let mut current_file: Option<File> = None;
        let mut current_file_path: Option<PathBuf> = None;
        
        {
            let file_map = listener.get_file_map_clone();
            
            if let Some((first_path, idr_pos)) = file_map.iter().next() {
                current_file_path = Some(first_path.clone());
                idr_pos_100 = idr_pos.clone();
                
                if let Ok(mut file) = File::open(first_path) {
                    if file.read_exact(unsafe {
                        std::slice::from_raw_parts_mut(
                            idr_pos_100.as_mut_ptr() as *mut u8,
                            MOOV_HEAD_LEN
                        )
                    }).is_ok() {
                        current_file = Some(file);
                    }
                }
            }
        }
        
        while *running.lock().unwrap() {
            let messages: Vec<u8> = {
                let mut list = message_list.lock().unwrap();
                std::mem::take(&mut *list)
            };
            
            for message in messages {
                match message {
                    0..=99 => {
                        if idr_pos_100[message as usize] >= MOOV_HEAD_LEN as i64 {
                            if let Some(ref mut file) = current_file {
                                let _ = file.seek(SeekFrom::Start(idr_pos_100[message as usize] as u64));
                                info!("Seek to position {} (message {})", idr_pos_100[message as usize], message);
                            }
                        }
                    }
                    101 => {
                        if progress > 1 {
                            if let Some(ref mut file) = current_file {
                                let _ = file.seek(SeekFrom::Start(idr_pos_100[(progress - 2) as usize] as u64));
                                info!("Fast back to position {}", idr_pos_100[(progress - 2) as usize]);
                            }
                        }
                    }
                    102 => {
                        if progress < 99 {
                            if let Some(ref mut file) = current_file {
                                let _ = file.seek(SeekFrom::Start(idr_pos_100[(progress + 1) as usize] as u64));
                                info!("Fast forward to position {}", idr_pos_100[(progress + 1) as usize]);
                            }
                        }
                    }
                    104 => {
                        let file_map = listener.get_file_map_clone();
                        
                        if let Some(ref current_path) = current_file_path {
                            let mut found = false;
                            let mut prev_path: Option<PathBuf> = None;
                            
                            for (path, idr_pos) in file_map.iter() {
                                if path == current_path {
                                    found = true;
                                    break;
                                }
                                prev_path = Some(path.clone());
                                idr_pos_100 = idr_pos.clone();
                            }
                            
                            if found {
                                if let Some(prev) = prev_path {
                                    current_file_path = Some(prev.clone());
                                    if let Ok(mut file) = File::open(&prev) {
                                        if file.read_exact(unsafe {
                                            std::slice::from_raw_parts_mut(
                                                idr_pos_100.as_mut_ptr() as *mut u8,
                                                MOOV_HEAD_LEN
                                            )
                                        }).is_ok() {
                                            current_file = Some(file);
                                            info!("Jumped to previous file: {:?}", prev);
                                        }
                                    }
                                }
                            }
                        }
                    }
                    105 => {
                        let file_map = listener.get_file_map_clone();
                        
                        if let Some(ref current_path) = current_file_path {
                            let mut found_current = false;
                            
                            for (path, idr_pos) in file_map.iter() {
                                if found_current {
                                    current_file_path = Some(path.clone());
                                    idr_pos_100 = idr_pos.clone();
                                    
                                    if let Ok(mut file) = File::open(path) {
                                        if file.read_exact(unsafe {
                                            std::slice::from_raw_parts_mut(
                                                idr_pos_100.as_mut_ptr() as *mut u8,
                                                MOOV_HEAD_LEN
                                            )
                                        }).is_ok() {
                                            current_file = Some(file);
                                            info!("Jumped to next file: {:?}", path);
                                        }
                                    }
                                    break;
                                }
                                if path == current_path {
                                    found_current = true;
                                }
                            }
                        }
                    }
                    107 => {
                        interval_ms = INTERVAL_FAST_PLAY_MS;
                        info!("Fast play started, interval: {}ms", interval_ms);
                    }
                    108 => {
                        interval_ms = INTERVAL_DEFAULT_MS;
                        info!("Fast play stopped, interval: {}ms", interval_ms);
                    }
                    _ => {
                        warn!("Unsupported message: {}", message);
                    }
                }
            }
            
            if let Some(ref mut file) = current_file {
                let mut len_bytes = [0u8; 4];
                match file.read_exact(&mut len_bytes) {
                    Ok(_) => {
                        // 读取4字节长度（小端）
                        let data_len = u32::from_le_bytes(len_bytes) as usize;
                        if data_len > 0 && data_len < TMP_BUFF_SIZE {
                            // 读取标志位 + 数据
                            let mut buffer = vec![0u8; data_len];
                            if file.read_exact(&mut buffer).is_ok() {
                                // 发送给客户端：4字节长度(网络字节序) + 1字节标志 + 数据
                                // 这与原camera项目格式一致
                                let send_len = data_len as u32;
                                let send_len_bytes = send_len.to_be_bytes();
                                
                                unsafe {
                                    // 发送长度
                                    let ret = libc::send(
                                        socket_fd,
                                        send_len_bytes.as_ptr() as *const libc::c_void,
                                        4,
                                        libc::MSG_NOSIGNAL,
                                    );
                                    if ret <= 0 {
                                        warn!("Failed to send length, client may have disconnected");
                                        break;
                                    }
                                    
                                    // 发送标志位+数据
                                    let ret = libc::send(
                                        socket_fd,
                                        buffer.as_ptr() as *const libc::c_void,
                                        buffer.len(),
                                        libc::MSG_NOSIGNAL,
                                    );
                                    if ret <= 0 {
                                        warn!("Failed to send data, client may have disconnected");
                                        break;
                                    }
                                }
                            }
                        }
                    }
                    Err(e) => {
                        if e.kind() == std::io::ErrorKind::UnexpectedEof {
                            let file_map = listener.get_file_map_clone();
                            
                            let mut found_current = false;
                            let mut next_file: Option<(PathBuf, Vec<i64>)> = None;
                            
                            for (path, idr_pos) in file_map.iter() {
                                if found_current {
                                    next_file = Some((path.clone(), idr_pos.clone()));
                                    break;
                                }
                                if current_file_path.as_ref() == Some(path) {
                                    found_current = true;
                                }
                            }
                            
                            if let Some((path, idr_pos)) = next_file {
                                current_file_path = Some(path.clone());
                                idr_pos_100 = idr_pos.clone();
                                
                                if let Ok(mut new_file) = File::open(&path) {
                                    if new_file.read_exact(unsafe {
                                        std::slice::from_raw_parts_mut(
                                            idr_pos_100.as_mut_ptr() as *mut u8,
                                            MOOV_HEAD_LEN
                                        )
                                    }).is_ok() {
                                        current_file = Some(new_file);
                                        info!("Reached EOF, opened next file: {:?}", path);
                                    }
                                }
                            } else {
                                info!("Reached end of all files");
                                break;
                            }
                        } else {
                            warn!("Read error: {:?}", e);
                            break;
                        }
                    }
                }
            }
            
            thread::sleep(Duration::from_millis(interval_ms));
        }
        
        info!("Send file task stopped for socket {}", socket_fd);
    }
    
    fn send_chunk(socket_fd: i32, data: &[u8]) -> Result<()> {
        let len = data.len() as u32;
        let len_bytes = len.to_be_bytes();
        
        unsafe {
            let ret = libc::send(
                socket_fd,
                len_bytes.as_ptr() as *const libc::c_void,
                4,
                libc::MSG_NOSIGNAL,
            );
            if ret <= 0 {
                return Err(anyhow::anyhow!("Failed to send length"));
            }
            
            let ret = libc::send(
                socket_fd,
                data.as_ptr() as *const libc::c_void,
                data.len(),
                libc::MSG_NOSIGNAL,
            );
            if ret <= 0 {
                return Err(anyhow::anyhow!("Failed to send data"));
            }
        }
        
        Ok(())
    }
    
    pub fn stop(&mut self) {
        *self.running.lock().unwrap() = false;
        
        if let Some(thread) = self.thread.take() {
            let _ = thread.join();
        }
        
        unsafe {
            libc::close(self.socket_fd);
        }
        
        info!("ClientConnect stopped for socket {}", self.socket_fd);
    }
}

impl Drop for ClientConnect {
    fn drop(&mut self) {
        self.stop();
    }
}
