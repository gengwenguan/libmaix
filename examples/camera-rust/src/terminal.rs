use anyhow::Result;
use std::net::SocketAddr;
use std::sync::Arc;
use std::thread::sleep;
use std::time::Duration;
use tracing::info;

use crate::camera::{Camera, CameraConfig};
use crate::encoder::{Encoder, h264::H264Encoder, opus::OpusEncoder};
use crate::server::tcp::{TcpServer, TcpServerListener};
use crate::server::file_mng::{FileMng, FileMngListener};
use crate::vo::{VideoOutput, VOConfig};

pub struct Terminal {
    camera: Camera,
    vo: VideoOutput,
    tcp_server: TcpServer,
    file_mng: FileMng,
    encoder: H264Encoder,
    opus_encoder: OpusEncoder,
    running: bool,
    force_keyframe_flag: Arc<std::sync::atomic::AtomicBool>,
}

/// TcpServer监听器实现
struct TerminalTcpListener {
    force_keyframe_flag: Arc<std::sync::atomic::AtomicBool>,
}

impl TcpServerListener for TerminalTcpListener {
    fn on_new_client_connect(&self, _fd: i32) {
        info!("New client connected, forcing keyframe");
        self.force_keyframe_flag.store(true, std::sync::atomic::Ordering::SeqCst);
    }
}

// 手动实现Send，因为Arc<AtomicBool>是Send安全的
unsafe impl Send for TerminalTcpListener {}
unsafe impl Sync for TerminalTcpListener {}

/// FileMng监听器实现
struct TerminalFileMngListener {
    force_keyframe_flag: Arc<std::sync::atomic::AtomicBool>,
}

impl FileMngListener for TerminalFileMngListener {
    fn on_new_file_create(&self) {
        info!("New file created, forcing keyframe");
        self.force_keyframe_flag.store(true, std::sync::atomic::Ordering::SeqCst);
    }
}

// 手动实现Send
unsafe impl Send for TerminalFileMngListener {}
unsafe impl Sync for TerminalFileMngListener {}

unsafe impl Send for Terminal {}
unsafe impl Sync for Terminal {}

impl Terminal {
    pub async fn new() -> Result<Self> {
        let camera = Camera::new()?;
        let camera_config = CameraConfig::default();
        camera.init(&camera_config)?;
        
        let vo = VideoOutput::new()?;
        let vo_config = VOConfig::default();
        vo.init(&vo_config)?;
        
        // 创建H264编码器
        let encoder = H264Encoder::new(camera.get_width(), camera.get_height())?;
        
        // 创建强制关键帧标志
        let force_keyframe_flag = Arc::new(std::sync::atomic::AtomicBool::new(false));
        
        // 创建TCP服务器并设置监听器
        let addr: SocketAddr = "0.0.0.0:56050".parse()?;
        let mut tcp_server = TcpServer::new(addr);
        
        // 设置新客户端连接回调
        let tcp_listener = Arc::new(TerminalTcpListener {
            force_keyframe_flag: force_keyframe_flag.clone(),
        });
        tcp_server.set_listener(tcp_listener);
        tcp_server.start().await?;
        
        // 创建文件管理器并设置监听器
        let file_mng_listener = Arc::new(TerminalFileMngListener {
            force_keyframe_flag: force_keyframe_flag.clone(),
        });
        let file_mng = FileMng::new(Some(file_mng_listener))?;
        
        let opus_encoder = OpusEncoder::new()?;
        
        Ok(Self {
            camera,
            vo,
            tcp_server,
            file_mng,
            encoder,
            opus_encoder,
            running: false,
            force_keyframe_flag,
        })
    }

    pub async fn run(&mut self) -> Result<()> {
        self.running = true;

        self.camera.start()?;
        self.vo.start()?;
        self.opus_encoder.start()?;

        info!("Terminal started");
        info!("Real-time stream server: port 56050");
        info!("Playback server: port 56060");

        while self.running {
            sleep(Duration::from_millis(3));

            let frame = self.vo.get_frame()?;
            let (vir, _phy) = self.vo.frame_addr(frame)?;

            self.camera.capture(vir as *mut ::std::os::raw::c_uchar)?;

            self.render_text(vir as *mut u8, self.camera.get_width(), self.camera.get_height())?;

            self.vo.set_frame(frame)?;

            // 检查是否需要强制关键帧
            if self.force_keyframe_flag.swap(false, std::sync::atomic::Ordering::SeqCst) {
                let _ = self.encoder.force_keyframe();
            }

            // 编码视频帧
            let encoded_data = self.encoder.encode(unsafe {
                std::slice::from_raw_parts(vir as *const u8, (self.camera.get_width() * self.camera.get_height() * 3 / 2) as usize)
            })?;

            if !encoded_data.is_empty() {
                self.tcp_server.send_h264_frame(&encoded_data).await?;
                self.file_mng.input_file_data(&encoded_data, 1);
            }

            if let Some(audio_data) = self.opus_encoder.get_encoded_data() {
                if !audio_data.is_empty() {
                    self.tcp_server.send_opus_data(&audio_data).await?;
                    self.file_mng.input_file_data(&audio_data, 0);
                }
            }
        }

        Ok(())
    }
    
    pub async fn stop(&mut self) -> Result<()> {
        self.running = false;

        self.camera.stop()?;
        self.vo.stop()?;
        self.opus_encoder.stop()?;
        self.tcp_server.stop()?;
        self.file_mng.stop();

        // 停止编码器
        self.encoder.stop()?;

        info!("Terminal stopped");

        Ok(())
    }
    
    pub fn get_ipv4_address() -> String {
        "127.0.0.1".to_string()
    }
    
    fn render_text(&self, buffer: *mut u8, width: u32, height: u32) -> Result<()> {
        let ip = Self::get_ipv4_address();
        let time = self.get_current_time();
        
        self.draw_string(buffer, width, height, &ip, 5, 30, 255)?;
        self.draw_string(buffer, width, height, &time, 5, height - 5, 255)?;
        
        Ok(())
    }
    
    fn get_current_time(&self) -> String {
        use chrono::Local;
        Local::now().format("%Y-%m-%d %H:%M:%S").to_string()
    }
    
    fn draw_string(&self, buffer: *mut u8, width: u32, height: u32, text: &str, x: u32, y: u32, brightness: u8) -> Result<()> {
        for (i, c) in text.chars().enumerate() {
            let char_x = x + (i as u32) * 8;
            if char_x + 8 > width {
                break;
            }
            self.draw_char(buffer, width, height, c, char_x, y, brightness)?;
        }
        Ok(())
    }
    
    fn draw_char(&self, buffer: *mut u8, width: u32, height: u32, _c: char, x: u32, y: u32, brightness: u8) -> Result<()> {
        for dy in 0..8 {
            for dx in 0..8 {
                let px = x + dx;
                let py = y + dy;
                if px < width && py < height {
                    let index = (py * width + px) as usize;
                    unsafe {
                        *buffer.add(index) = brightness;
                    }
                }
            }
        }
        Ok(())
    }
}
