use crate::config::AppConfig;
use anyhow::{anyhow, Context, Result};
use std::ffi::{c_char, c_void, CStr, CString};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::ptr::NonNull;
use std::sync::Arc;

#[repr(C)]
struct CameraNative {
    _private: [u8; 0],
}
#[repr(C)]
struct CameraTlsContext {
    _private: [u8; 0],
}
#[repr(C)]
struct CameraTlsConnection {
    _private: [u8; 0],
}

type BytesCallback = unsafe extern "C" fn(*mut c_void, *const u8, usize);
type FloatCallback = unsafe extern "C" fn(*mut c_void, f32);
type VideoCallback = unsafe extern "C" fn(*mut c_void, *const u8, usize, i64, u8);

#[repr(C)]
struct NativeCallbacks {
    opaque: *mut c_void,
    on_init_segment: Option<BytesCallback>,
    on_fragment: Option<BytesCallback>,
    on_audio_adts: Option<VideoCallback>,
    on_log_line: Option<BytesCallback>,
    on_nv21_frame: Option<BytesCallback>,
    on_person_detected: Option<FloatCallback>,
    on_h264_access_unit: Option<VideoCallback>,
    on_opus_frame: Option<VideoCallback>,
}

#[repr(C)]
struct NativeRuntimeConfig {
    ai_enabled: u8,
    ai_threshold: f32,
    ai_min_interval_s: i32,
    ai_infer_fps: i32,
    album_max_photos: i32,
    photo_jpeg_qual: i32,
    mic_filter_mode: i32,
    osd_show_ip: u8,
    osd_show_time: u8,
    osd_show_ai_box: u8,
}

unsafe extern "C" {
    fn camera_native_create(
        runtime_dir: *const c_char,
        callbacks: *const NativeCallbacks,
    ) -> *mut CameraNative;
    fn camera_native_start(camera: *mut CameraNative) -> i32;
    fn camera_native_stop(camera: *mut CameraNative);
    fn camera_native_destroy(camera: *mut CameraNative);
    fn camera_native_force_iframe(camera: *mut CameraNative);
    fn camera_native_webrtc_set_active(camera: *mut CameraNative, active: i32);
    fn camera_native_remote_video_set_active(camera: *mut CameraNative, active: i32);
    fn camera_native_config_set(
        camera: *mut CameraNative,
        config: *const NativeRuntimeConfig,
    ) -> i32;
    fn camera_native_encode_jpeg(
        camera: *mut CameraNative,
        nv21: *const u8,
        len: usize,
        quality: i32,
        output: *mut *mut u8,
        output_len: *mut usize,
    ) -> i32;
    fn camera_native_talk_open(camera: *mut CameraNative) -> i32;
    fn camera_native_talk_close(camera: *mut CameraNative);
    fn camera_native_talk_feed(camera: *mut CameraNative, data: *const u8, len: usize) -> i32;
    fn camera_native_play_prompt_pcm(
        camera: *mut CameraNative,
        samples: *const i16,
        frames: usize,
    ) -> i32;
    fn camera_native_mic_loudness(camera: *mut CameraNative) -> i32;
    fn camera_native_free(ptr: *mut c_void);
    fn camera_native_last_error(camera: *mut CameraNative) -> *const c_char;
    fn camera_tls_context_create(
        cert_path: *const c_char,
        key_path: *const c_char,
    ) -> *mut CameraTlsContext;
    fn camera_tls_context_destroy(context: *mut CameraTlsContext);
    fn camera_tls_accept(context: *mut CameraTlsContext, fd: i32) -> *mut CameraTlsConnection;
    fn camera_tls_read(
        connection: *mut CameraTlsConnection,
        data: *mut c_void,
        len: i32,
        want_more: *mut i32,
    ) -> i32;
    fn camera_tls_write(
        connection: *mut CameraTlsConnection,
        data: *const c_void,
        len: i32,
        want_more: *mut i32,
    ) -> i32;
    fn camera_tls_connection_destroy(connection: *mut CameraTlsConnection);
}

pub trait MediaCallbacks: Send + Sync + 'static {
    fn on_init_segment(&self, data: &[u8]);
    fn on_fragment(&self, data: &[u8]);
    fn on_audio_adts(&self, data: &[u8], pts_us: i64);
    fn on_log_line(&self, data: &[u8]);
    fn on_nv21_frame(&self, data: &[u8]);
    fn on_person_detected(&self, probability: f32);
    fn on_h264_access_unit(&self, data: &[u8], pts_us: i64, is_key: bool);
    fn on_opus_frame(&self, data: &[u8], pts_us: i64);
}

struct CallbackContext {
    callbacks: Arc<dyn MediaCallbacks>,
}

unsafe extern "C" fn on_init(opaque: *mut c_void, data: *const u8, len: usize) {
    invoke_callback(opaque, data, len, |callbacks, bytes| {
        callbacks.on_init_segment(bytes)
    });
}

unsafe extern "C" fn on_fragment(opaque: *mut c_void, data: *const u8, len: usize) {
    invoke_callback(opaque, data, len, |callbacks, bytes| {
        callbacks.on_fragment(bytes)
    });
}

unsafe extern "C" fn on_audio(
    opaque: *mut c_void,
    data: *const u8,
    len: usize,
    pts_us: i64,
    _flags: u8,
) {
    invoke_callback(opaque, data, len, |callbacks, bytes| {
        callbacks.on_audio_adts(bytes, pts_us)
    });
}

unsafe extern "C" fn on_log(opaque: *mut c_void, data: *const u8, len: usize) {
    invoke_callback(opaque, data, len, |callbacks, bytes| {
        callbacks.on_log_line(bytes)
    });
}

unsafe extern "C" fn on_nv21_frame(opaque: *mut c_void, data: *const u8, len: usize) {
    invoke_callback(opaque, data, len, |callbacks, bytes| {
        callbacks.on_nv21_frame(bytes)
    });
}

unsafe extern "C" fn on_person_detected(opaque: *mut c_void, probability: f32) {
    if opaque.is_null() {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        let context = unsafe { &*(opaque.cast::<CallbackContext>()) };
        context.callbacks.on_person_detected(probability);
    }));
}

unsafe extern "C" fn on_h264_access_unit(
    opaque: *mut c_void,
    data: *const u8,
    len: usize,
    pts_us: i64,
    is_key: u8,
) {
    invoke_callback(opaque, data, len, |callbacks, bytes| {
        callbacks.on_h264_access_unit(bytes, pts_us, is_key != 0)
    });
}

unsafe extern "C" fn on_opus_frame(
    opaque: *mut c_void,
    data: *const u8,
    len: usize,
    pts_us: i64,
    _flags: u8,
) {
    invoke_callback(opaque, data, len, |callbacks, bytes| {
        callbacks.on_opus_frame(bytes, pts_us)
    });
}

unsafe fn invoke_callback(
    opaque: *mut c_void,
    data: *const u8,
    len: usize,
    callback: impl FnOnce(&dyn MediaCallbacks, &[u8]),
) {
    if opaque.is_null() || data.is_null() || len == 0 {
        return;
    }
    // Never allow a Rust panic to unwind through a C++ media thread.
    let _ = catch_unwind(AssertUnwindSafe(|| {
        let context = unsafe { &*(opaque.cast::<CallbackContext>()) };
        let bytes = unsafe { std::slice::from_raw_parts(data, len) };
        callback(context.callbacks.as_ref(), bytes);
    }));
}

pub struct NativeEngine {
    handle: NonNull<CameraNative>,
    // The C++ side keeps this address as callback opaque data.
    callback_context: Box<CallbackContext>,
}

// The bridge exposes synchronized operations only. Capture ownership remains in
// its native thread; callers never access vendor pointers directly.
unsafe impl Send for NativeEngine {}
unsafe impl Sync for NativeEngine {}

impl NativeEngine {
    pub fn new(runtime_dir: &str, callbacks: Arc<dyn MediaCallbacks>) -> Result<Self> {
        let runtime_dir = CString::new(runtime_dir).context("runtime dir contains NUL")?;
        let mut callback_context = Box::new(CallbackContext { callbacks });
        let native_callbacks = NativeCallbacks {
            opaque: (&mut *callback_context as *mut CallbackContext).cast(),
            on_init_segment: Some(on_init),
            on_fragment: Some(on_fragment),
            on_audio_adts: Some(on_audio),
            on_log_line: Some(on_log),
            on_nv21_frame: Some(on_nv21_frame),
            on_person_detected: Some(on_person_detected),
            on_h264_access_unit: Some(on_h264_access_unit),
            on_opus_frame: Some(on_opus_frame),
        };
        let handle = unsafe { camera_native_create(runtime_dir.as_ptr(), &native_callbacks) };
        let handle = NonNull::new(handle).ok_or_else(|| anyhow!("camera_native_create failed"))?;
        Ok(Self {
            handle,
            callback_context,
        })
    }

    pub fn start(&self) -> Result<()> {
        let result = unsafe { camera_native_start(self.handle.as_ptr()) };
        if result == 0 {
            Ok(())
        } else {
            Err(anyhow!(self.last_error()))
        }
    }

    pub fn stop(&self) {
        unsafe { camera_native_stop(self.handle.as_ptr()) }
    }

    pub fn force_iframe(&self) {
        unsafe { camera_native_force_iframe(self.handle.as_ptr()) }
    }

    pub fn set_webrtc_active(&self, active: bool) {
        unsafe { camera_native_webrtc_set_active(self.handle.as_ptr(), i32::from(active)) }
    }

    pub fn set_remote_video_active(&self, active: bool) {
        unsafe { camera_native_remote_video_set_active(self.handle.as_ptr(), i32::from(active)) }
    }

    pub fn set_config(&self, config: &AppConfig) -> Result<()> {
        let native = NativeRuntimeConfig {
            ai_enabled: u8::from(config.ai_enabled),
            ai_threshold: config.ai_threshold as f32,
            ai_min_interval_s: config.ai_min_interval_s,
            ai_infer_fps: config.ai_infer_fps,
            album_max_photos: config.album_max_photos,
            photo_jpeg_qual: config.photo_jpeg_qual,
            mic_filter_mode: config.mic_filter_mode,
            osd_show_ip: u8::from(config.osd_show_ip),
            osd_show_time: u8::from(config.osd_show_time),
            osd_show_ai_box: u8::from(config.osd_show_ai_box),
        };
        if unsafe { camera_native_config_set(self.handle.as_ptr(), &native) } != 0 {
            return Err(anyhow!("native config update failed"));
        }
        Ok(())
    }

    pub fn encode_jpeg(&self, nv21: &[u8], quality: i32) -> Result<Vec<u8>> {
        let mut output = std::ptr::null_mut();
        let mut output_len = 0usize;
        let result = unsafe {
            camera_native_encode_jpeg(
                self.handle.as_ptr(),
                nv21.as_ptr(),
                nv21.len(),
                quality,
                &mut output,
                &mut output_len,
            )
        };
        if result != 0 || output.is_null() || output_len == 0 {
            return Err(anyhow!("native JPEG encoding failed"));
        }
        let bytes = unsafe { std::slice::from_raw_parts(output, output_len) }.to_vec();
        unsafe { camera_native_free(output.cast()) };
        Ok(bytes)
    }

    pub fn talk_open(&self) -> Result<()> {
        match unsafe { camera_native_talk_open(self.handle.as_ptr()) } {
            0 => Ok(()),
            _ => Err(anyhow!("talk device unavailable")),
        }
    }

    pub fn talk_close(&self) {
        unsafe { camera_native_talk_close(self.handle.as_ptr()) }
    }

    pub fn talk_play_frame(&self, data: &[u8]) -> Result<()> {
        match unsafe { camera_native_talk_feed(self.handle.as_ptr(), data.as_ptr(), data.len()) } {
            0 => Ok(()),
            code => Err(anyhow!("talk frame playback failed: {code}")),
        }
    }

    pub fn play_prompt_pcm(&self, samples: &[i16]) -> Result<()> {
        if samples.is_empty() {
            return Err(anyhow!("prompt PCM is empty"));
        }
        match unsafe {
            camera_native_play_prompt_pcm(self.handle.as_ptr(), samples.as_ptr(), samples.len())
        } {
            0 => Ok(()),
            -3 => Err(anyhow!("prompt player is busy")),
            code => Err(anyhow!("prompt playback failed: {code}")),
        }
    }

    pub fn mic_loudness(&self) -> i32 {
        unsafe { camera_native_mic_loudness(self.handle.as_ptr()) }
    }

    fn last_error(&self) -> String {
        let ptr = unsafe { camera_native_last_error(self.handle.as_ptr()) };
        if ptr.is_null() {
            "unknown native error".to_owned()
        } else {
            unsafe { CStr::from_ptr(ptr) }
                .to_string_lossy()
                .into_owned()
        }
    }
}

impl Drop for NativeEngine {
    fn drop(&mut self) {
        unsafe {
            camera_native_stop(self.handle.as_ptr());
            camera_native_destroy(self.handle.as_ptr());
        }
        // Keep the callback context observably used until native destruction.
        let _ = &self.callback_context;
    }
}

pub struct TlsContext {
    handle: NonNull<CameraTlsContext>,
}

unsafe impl Send for TlsContext {}
unsafe impl Sync for TlsContext {}

impl TlsContext {
    pub fn new(cert_path: &str, key_path: &str) -> Result<Self> {
        let cert_path = CString::new(cert_path).context("certificate path contains NUL")?;
        let key_path = CString::new(key_path).context("private key path contains NUL")?;
        let handle = unsafe { camera_tls_context_create(cert_path.as_ptr(), key_path.as_ptr()) };
        let handle = NonNull::new(handle).ok_or_else(|| anyhow!("TLS context init failed"))?;
        Ok(Self { handle })
    }

    pub fn accept(&self, fd: i32) -> Result<TlsConnection> {
        let handle = unsafe { camera_tls_accept(self.handle.as_ptr(), fd) };
        let handle = NonNull::new(handle).ok_or_else(|| anyhow!("TLS handshake failed"))?;
        Ok(TlsConnection { handle })
    }
}

impl Drop for TlsContext {
    fn drop(&mut self) {
        unsafe { camera_tls_context_destroy(self.handle.as_ptr()) }
    }
}

pub struct TlsConnection {
    handle: NonNull<CameraTlsConnection>,
}

unsafe impl Send for TlsConnection {}

impl TlsConnection {
    pub fn read(&mut self, output: &mut [u8]) -> std::io::Result<Option<usize>> {
        let len = output.len().min(i32::MAX as usize) as i32;
        let mut want_more = 0;
        let result = unsafe {
            camera_tls_read(
                self.handle.as_ptr(),
                output.as_mut_ptr().cast(),
                len,
                &mut want_more,
            )
        };
        match result {
            value if value > 0 => Ok(Some(value as usize)),
            0 => Ok(Some(0)),
            _ if want_more != 0 => Ok(None),
            _ => Err(std::io::Error::other("TLS read failed")),
        }
    }

    pub fn write(&mut self, input: &[u8]) -> std::io::Result<Option<usize>> {
        let len = input.len().min(i32::MAX as usize) as i32;
        let mut want_more = 0;
        let result = unsafe {
            camera_tls_write(
                self.handle.as_ptr(),
                input.as_ptr().cast(),
                len,
                &mut want_more,
            )
        };
        match result {
            value if value > 0 => Ok(Some(value as usize)),
            _ if want_more != 0 => Ok(None),
            _ => Err(std::io::Error::other("TLS write failed")),
        }
    }
}

impl Drop for TlsConnection {
    fn drop(&mut self) {
        unsafe { camera_tls_connection_destroy(self.handle.as_ptr()) }
    }
}
