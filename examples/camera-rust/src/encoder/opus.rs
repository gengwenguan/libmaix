use anyhow::{Result, bail};
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::thread;
use std::sync::mpsc;
use tracing::{info, warn, error};

// 导入绑定
include!(concat!(env!("OUT_DIR"), "/bindings.rs"));

const SAMPLE_RATE: u32 = 48000;
const PERIOD_SIZE: usize = 960; // 20ms at 48kHz
const CHANNELS: u32 = 1;
const BIT_RATE: i64 = 64000;

// FFmpeg常量
const AV_SAMPLE_FMT_S16: i32 = 1;
const AVERROR_EAGAIN: i32 = -11;
const AVERROR_EOF: i32 = -541478725;

/// Opus音频编码器
pub struct OpusEncoder {
    running: Arc<AtomicBool>,
    thread: Option<thread::JoinHandle<()>>,
    tx: mpsc::Sender<Vec<u8>>,
    rx: mpsc::Receiver<Vec<u8>>,
}

impl OpusEncoder {
    pub fn new() -> Result<Self> {
        let (tx, rx) = mpsc::channel();
        
        Ok(Self {
            running: Arc::new(AtomicBool::new(false)),
            thread: None,
            tx,
            rx,
        })
    }
    
    pub fn start(&mut self) -> Result<()> {
        if self.running.load(Ordering::Relaxed) {
            bail!("Opus encoder already running");
        }
        
        self.running.store(true, Ordering::Relaxed);
        
        let tx = self.tx.clone();
        let running = self.running.clone();
        
        let thread = thread::spawn(move || {
            if let Err(e) = Self::capture_and_encode(tx, running) {
                error!("Opus capture and encode error: {:?}", e);
            }
        });
        
        self.thread = Some(thread);
        info!("Opus encoder started");
        Ok(())
    }
    
    pub fn stop(&mut self) -> Result<()> {
        self.running.store(false, Ordering::Relaxed);
        
        if let Some(thread) = self.thread.take() {
            thread.join().unwrap_or_else(|e| {
                warn!("Opus encoder thread join failed: {:?}", e);
            });
        }
        
        info!("Opus encoder stopped");
        Ok(())
    }
    
    pub fn get_encoded_data(&mut self) -> Option<Vec<u8>> {
        self.rx.try_recv().ok()
    }
    
    /// 采集和编码音频数据
    fn capture_and_encode(tx: mpsc::Sender<Vec<u8>>, running: Arc<AtomicBool>) -> Result<()> {
        info!("Opus capture and encode thread started");
        
        unsafe {
            // 初始化ALSA
            let mut capture_handle: *mut snd_pcm_t = std::ptr::null_mut();
            let mut hw_params: *mut snd_pcm_hw_params_t = std::ptr::null_mut();
            
            // 打开音频设备
            let ret = snd_pcm_open(
                &mut capture_handle,
                b"default\0".as_ptr() as *const ::std::os::raw::c_char,
                _snd_pcm_stream_SND_PCM_STREAM_CAPTURE,
                0
            );
            if ret < 0 {
                bail!("Failed to open PCM device: {}", ret);
            }
            
            // 分配硬件参数
            snd_pcm_hw_params_malloc(&mut hw_params);
            snd_pcm_hw_params_any(capture_handle, hw_params);
            snd_pcm_hw_params_set_access(
                capture_handle, 
                hw_params, 
                _snd_pcm_access_SND_PCM_ACCESS_RW_INTERLEAVED
            );
            snd_pcm_hw_params_set_format(
                capture_handle, 
                hw_params, 
                _snd_pcm_format_SND_PCM_FORMAT_S16_LE
            );
            
            let mut sample_rate = SAMPLE_RATE;
            snd_pcm_hw_params_set_rate_near(capture_handle, hw_params, &mut sample_rate, std::ptr::null_mut());
            snd_pcm_hw_params_set_channels(capture_handle, hw_params, CHANNELS);
            
            let mut period_size = PERIOD_SIZE as snd_pcm_uframes_t;
            snd_pcm_hw_params_set_period_size_near(capture_handle, hw_params, &mut period_size, std::ptr::null_mut());
            
            snd_pcm_hw_params(capture_handle, hw_params);
            snd_pcm_hw_params_free(hw_params);
            snd_pcm_prepare(capture_handle);
            
            // 初始化FFmpeg Opus编码器
            avcodec_register_all();
            avdevice_register_all();
            
            let codec = avcodec_find_encoder(AVCodecID_AV_CODEC_ID_OPUS);
            if codec.is_null() {
                snd_pcm_close(capture_handle);
                bail!("Could not find Opus codec");
            }
            
            let mut codec_ctx = avcodec_alloc_context3(codec);
            if codec_ctx.is_null() {
                snd_pcm_close(capture_handle);
                bail!("Could not allocate codec context");
            }
            
            (*codec_ctx).sample_rate = SAMPLE_RATE as i32;
            (*codec_ctx).channel_layout = AV_CH_LAYOUT_MONO as u64;
            (*codec_ctx).channels = CHANNELS as i32;
            (*codec_ctx).sample_fmt = AV_SAMPLE_FMT_S16;
            (*codec_ctx).bit_rate = BIT_RATE;
            
            let ret = avcodec_open2(codec_ctx, codec, std::ptr::null_mut());
            if ret < 0 {
                avcodec_free_context(&mut codec_ctx);
                snd_pcm_close(capture_handle);
                bail!("Could not open codec: {}", ret);
            }
            
            // 分配AVFrame
            let mut frame = av_frame_alloc();
            if frame.is_null() {
                avcodec_close(codec_ctx);
                avcodec_free_context(&mut codec_ctx);
                snd_pcm_close(capture_handle);
                bail!("Could not allocate frame");
            }
            
            (*frame).nb_samples = PERIOD_SIZE as i32;
            (*frame).format = AV_SAMPLE_FMT_S16;
            (*frame).channel_layout = AV_CH_LAYOUT_MONO as u64;
            (*frame).channels = CHANNELS as i32;
            (*frame).sample_rate = SAMPLE_RATE as i32;
            
            let ret = av_frame_get_buffer(frame, 0);
            if ret < 0 {
                av_frame_free(&mut frame);
                avcodec_close(codec_ctx);
                avcodec_free_context(&mut codec_ctx);
                snd_pcm_close(capture_handle);
                bail!("Could not allocate frame buffer: {}", ret);
            }
            
            let mut pkt = av_packet_alloc();
            if pkt.is_null() {
                av_frame_free(&mut frame);
                avcodec_close(codec_ctx);
                avcodec_free_context(&mut codec_ctx);
                snd_pcm_close(capture_handle);
                bail!("Could not allocate packet");
            }
            
            // 采集缓冲区
            let mut capture_buffer: Vec<i16> = vec![0; PERIOD_SIZE * CHANNELS as usize];
            let mut frame_count = 0;
            
            while running.load(Ordering::Relaxed) {
                // 读取音频数据
                let ret = snd_pcm_readi(
                    capture_handle,
                    capture_buffer.as_mut_ptr() as *mut std::ffi::c_void,
                    PERIOD_SIZE as snd_pcm_uframes_t
                );
                
                if ret == -32 { // -EPIPE
                    warn!("ALSA underrun occurred");
                    snd_pcm_prepare(capture_handle);
                    continue;
                }
                
                if ret < 0 {
                    error!("ALSA read error: {}", ret);
                    continue;
                }
                
                // 设置帧数据
                std::ptr::copy_nonoverlapping(
                    capture_buffer.as_ptr(),
                    (*frame).data[0] as *mut i16,
                    PERIOD_SIZE * CHANNELS as usize
                );
                (*frame).nb_samples = PERIOD_SIZE as i32;
                
                // 发送帧到编码器
                let ret = avcodec_send_frame(codec_ctx, frame);
                if ret < 0 {
                    error!("Error sending frame to encoder: {}", ret);
                    continue;
                }
                
                // 接收编码后的数据
                loop {
                    let ret = avcodec_receive_packet(codec_ctx, pkt);
                    if ret == AVERROR_EAGAIN || ret == AVERROR_EOF {
                        break;
                    }
                    if ret < 0 {
                        error!("Error receiving packet from encoder: {}", ret);
                        break;
                    }
                    
                    // 复制编码后的数据
                    let data = std::slice::from_raw_parts((*pkt).data, (*pkt).size as usize);
                    let encoded_data = data.to_vec();
                    
                    // 释放packet
                    av_packet_unref(pkt);
                    
                    // 发送编码后的数据
                    if let Err(e) = tx.send(encoded_data) {
                        error!("Failed to send encoded audio data: {:?}", e);
                        break;
                    }
                }
                
                frame_count += 1;
                if frame_count % 100 == 0 {
                    info!("Encoded {} audio frames", frame_count);
                }
            }
            
            // 清理资源
            av_packet_free(&mut pkt);
            av_frame_free(&mut frame);
            avcodec_close(codec_ctx);
            avcodec_free_context(&mut codec_ctx);
            snd_pcm_close(capture_handle);
        }
        
        info!("Opus capture and encode thread stopped");
        Ok(())
    }
}

impl Drop for OpusEncoder {
    fn drop(&mut self) {
        let _ = self.stop();
    }
}
