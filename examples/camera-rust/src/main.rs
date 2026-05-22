mod camera;
mod encoder;
mod server;
mod terminal;
mod vo;
mod memory;
mod nal;

use anyhow::Result;
use tokio::signal;
use std::sync::Arc;
use tokio::sync::Mutex;
use std::thread::sleep as thread_sleep;
use std::time::Duration;
use tracing::info;
use std::net::SocketAddr;
use encoder::Encoder;

// 导入绑定
include!(concat!(env!("OUT_DIR"), "/bindings.rs"));

fn main() {
    tokio::runtime::Builder::new_multi_thread()
        .enable_all()
        .build()
        .unwrap()
        .block_on(async {
            if let Err(e) = run().await {
                eprintln!("Error: {:?}", e);
            }
        });
}

async fn run() -> Result<()> {
    // 初始化日志
    tracing_subscriber::fmt()
        .with_env_filter(tracing_subscriber::EnvFilter::from_default_env())
        .init();

    info!("main enter!");
    
    // 等待8秒让设备获取IP地址和时间
    info!("Waiting for 8 seconds to get IP address and time...");
    thread_sleep(Duration::from_secs(8));

    // 初始化模块
    unsafe {
        let ret = libmaix_image_module_init();
        if ret != libmaix_err_t_LIBMAIX_ERR_NONE {
            anyhow::bail!("Failed to initialize image module: {}", ret);
        }
        
        libmaix_camera_module_init();
    }

    // 创建相机
    let camera = Arc::new(Mutex::new(camera::Camera::new()?));
    let camera_config = camera::CameraConfig::default();
    camera.lock().await.init(&camera_config)?;
    camera.lock().await.start()?;

    // 获取相机分辨率
    let width = camera.lock().await.get_width();
    let height = camera.lock().await.get_height();
    info!("Camera initialized with resolution: {}x{}", width, height);

    // 创建 H264 编码器
    let encoder = Arc::new(Mutex::new(encoder::h264::H264Encoder::new(width, height)?));
    info!("H264 encoder initialized");

    // 强制关键帧以获取 SPS/PPS 数据
    encoder.lock().await.force_keyframe()?;
    info!("Forced keyframe to get SPS/PPS data");

    // 创建 TCP 服务器
    let addr: SocketAddr = "0.0.0.0:8080".parse()?;
    let tcp_server = Arc::new(Mutex::new(server::tcp::TcpServer::new(addr)));
    tcp_server.lock().await.start().await?;
    info!("TCP server started on {}", addr);

    // 启动 TCP 服务器线程
    let tcp_server_clone = tcp_server.clone();
    tokio::spawn(async move {
        if let Err(e) = tcp_server_clone.lock().await.run().await {
            eprintln!("TCP server error: {:?}", e);
        }
    });

    // 分配帧缓冲区
    let frame_size = (width * height * 3 / 2) as usize; // YUV420
    let mut frame_buffer = vec![0u8; frame_size];

    // 主循环：采集→编码→传输
    let mut frame_count = 0;
    let mut last_keyframe_time = std::time::Instant::now();
    
    // 等待 Ctrl+C 信号
    let ctrl_c = tokio::signal::ctrl_c();
    tokio::select! {
        _ = ctrl_c => {
            info!("Received Ctrl+C, shutting down...");
        }
        _ = async {
            loop {
                // 采集帧
                { 
                    let camera = camera.lock().await;
                    if let Err(e) = camera.capture(frame_buffer.as_mut_ptr()) {
                        eprintln!("Camera capture error: {:?}", e);
                        break;
                    }
                }

                // 每30帧强制一次关键帧
                frame_count += 1;
                if frame_count % 30 == 0 || last_keyframe_time.elapsed() > Duration::from_secs(10) {
                    if let Err(e) = encoder.lock().await.force_keyframe() {
                        eprintln!("Encoder force keyframe error: {:?}", e);
                    }
                    last_keyframe_time = std::time::Instant::now();
                    info!("Forced keyframe at frame {}", frame_count);
                }

                // 编码帧
                let encoded_data = match encoder.lock().await.encode(&frame_buffer) {
                    Ok(data) => data,
                    Err(e) => {
                        eprintln!("Encoder error: {:?}", e);
                        continue;
                    }
                };

                // 传输编码数据
                if !encoded_data.is_empty() {
                    if let Err(e) = tcp_server.lock().await.send_h264_frame(&encoded_data).await {
                        eprintln!("TCP send error: {:?}", e);
                    }
                }

                // 控制帧率
                tokio::time::sleep(Duration::from_millis(33)).await; // ~30fps
            }
        } => {}
    }

    // 反初始化模块
    unsafe {
        libmaix_camera_module_deinit();
        libmaix_image_module_deinit();
    }

    info!("main end!");

    Ok(())
}
