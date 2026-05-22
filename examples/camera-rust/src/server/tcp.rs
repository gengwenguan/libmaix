use anyhow::Result;
use std::net::SocketAddr;
use tokio::net::{TcpListener, TcpStream};
use tokio::io::AsyncWriteExt;
use tokio::sync::Mutex;
use tracing::info;
use std::sync::Arc;
use crate::nal::NALParser;

// 标志位定义，与原camera项目对齐
const FLAG_AUDIO: u8 = 0x00;  // 音频标志 0<<7
const FLAG_VIDEO: u8 = 0x80;  // 视频标志 1<<7

/// TcpServer监听器trait，用于通知新客户端连接
pub trait TcpServerListener: Send + Sync {
    fn on_new_client_connect(&self, fd: i32);
}

pub struct TcpServer {
    addr: SocketAddr,
    listener: Option<TcpListener>,
    clients: Arc<Mutex<Vec<TcpStream>>>,
    listener_callback: Option<Arc<dyn TcpServerListener>>,
}

impl TcpServer {
    pub fn new(addr: SocketAddr) -> Self {
        Self {
            addr,
            listener: None,
            clients: Arc::new(Mutex::new(Vec::new())),
            listener_callback: None,
        }
    }
    
    /// 设置监听器回调
    pub fn set_listener(&mut self, listener: Arc<dyn TcpServerListener>) {
        self.listener_callback = Some(listener);
    }
    
    pub async fn start(&mut self) -> Result<()> {
        let listener = TcpListener::bind(self.addr).await?;
        self.listener = Some(listener);
        info!("TCP server started on {}", self.addr);
        
        Ok(())
    }
    
    pub async fn run(&self) -> Result<()> {
        let listener = self.listener.as_ref().ok_or_else(|| anyhow::anyhow!("Server not started"))?;
        let clients = self.clients.clone();
        let callback = self.listener_callback.clone();
        
        loop {
            let (socket, addr) = listener.accept().await?;
            info!("Client connected: {}", addr);
            
            let clients = clients.clone();
            let callback = callback.clone();
            tokio::spawn(async move {
                // 获取socket文件描述符
                let fd = socket.as_raw_fd();
                Self::handle_client(socket, clients).await;
                
                // 通知监听器有新客户端连接
                if let Some(cb) = callback {
                    cb.on_new_client_connect(fd);
                }
            });
        }
    }

    async fn handle_client(socket: TcpStream, clients: Arc<Mutex<Vec<TcpStream>>>) {
        let addr = socket.peer_addr().unwrap_or_else(|_| "unknown".parse().unwrap());
        
        {
            let mut clients = clients.lock().await;
            clients.push(socket);
        }
        
        info!("Client {} connected and added to client list", addr);
    }
    
    pub fn stop(&mut self) -> Result<()> {
        self.listener = None;
        Ok(())
    }
    
    /// 发送媒体数据，与原camera项目对齐格式
    /// 格式：4字节长度(网络字节序) + 1字节标志 + 数据
    async fn send_media(&self, data: &[u8], flag: u8) -> Result<()> {
        let mut clients = self.clients.lock().await;
        
        clients.retain(|client| {
            client.peer_addr().is_ok()
        });
        
        let data_len = data.len() + 1; // +1 for flag byte
        let len_bytes = (data_len as u32).to_be_bytes();
        
        for client in clients.iter_mut() {
            if let Err(e) = client.write_all(&len_bytes).await {
                info!("Error sending length to client: {:?}", e);
                continue;
            }
            if let Err(e) = client.write_all(&[flag]).await {
                info!("Error sending flag to client: {:?}", e);
                continue;
            }
            if let Err(e) = client.write_all(data).await {
                info!("Error sending data to client: {:?}", e);
            }
        }
        
        Ok(())
    }
    
    /// 发送H264视频帧，使用视频标志
    pub async fn send_h264_frame(&self, data: &[u8]) -> Result<()> {
        let nal_type = NALParser::get_nal_type(data);
        info!("Sending H264 frame, NAL type: {:?}, size: {}", nal_type, data.len());
        
        self.send_media(data, FLAG_VIDEO).await
    }
    
    /// 发送Opus音频数据，使用音频标志
    pub async fn send_opus_data(&self, data: &[u8]) -> Result<()> {
        info!("Sending Opus audio data, size: {}", data.len());
        
        self.send_media(data, FLAG_AUDIO).await
    }
}

use std::os::unix::io::AsRawFd;
