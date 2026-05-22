use anyhow::Result;

// 导入绑定
include!(concat!(env!("OUT_DIR"), "/bindings.rs"));

pub struct Camera {
    cam: *mut libmaix_cam_t,
    width: u32,
    height: u32,
}

impl Camera {
    pub fn new() -> Result<Self> {
        let width = 640u32;
        let height = 480u32;
        let cam = unsafe {
            libmaix_cam_create(0, width as i32, height as i32, 1, 0)
        };
        
        if cam.is_null() {
            anyhow::bail!("Failed to create camera");
        }
        
        Ok(Self { cam, width, height })
    }
    
    pub fn init(&self, _config: &CameraConfig) -> Result<()> {
        // 启动摄像头采集
        unsafe {
            let start_capture = (*self.cam).start_capture;
            if let Some(start_capture) = start_capture {
                let ret = start_capture(self.cam);
                if ret != libmaix_err_t_LIBMAIX_ERR_NONE {
                    anyhow::bail!("Failed to start camera capture: {}", ret);
                }
            } else {
                anyhow::bail!("start_capture method not available");
            }
        }
        Ok(())
    }
    
    pub fn start(&self) -> Result<()> {
        // 已经在init中启动
        Ok(())
    }
    
    pub fn stop(&self) -> Result<()> {
        // 相机在销毁时会自动停止
        Ok(())
    }
    
    pub fn capture(&self, buffer: *mut ::std::os::raw::c_uchar) -> Result<()> {
        unsafe {
            let capture = (*self.cam).capture;
            if let Some(capture) = capture {
                let ret = capture(self.cam, buffer);
                if ret != libmaix_err_t_LIBMAIX_ERR_NONE {
                    anyhow::bail!("Failed to capture frame: {}", ret);
                }
            } else {
                anyhow::bail!("capture method not available");
            }
        }
        Ok(())
    }
    
    pub fn get_width(&self) -> u32 {
        self.width
    }
    
    pub fn get_height(&self) -> u32 {
        self.height
    }
}

impl Drop for Camera {
    fn drop(&mut self) {
        unsafe {
            libmaix_cam_destroy(&mut self.cam);
        }
    }
}

// 为Camera实现Send和Sync trait，因为它包含原始指针
unsafe impl Send for Camera {}
unsafe impl Sync for Camera {}

pub struct CameraConfig {
    pub dev: &'static str,
    pub width: u32,
    pub height: u32,
    pub format: u32,
    pub fps: u32,
    pub rotation: u32,
}

impl Default for CameraConfig {
    fn default() -> Self {
        Self {
            dev: "/dev/video0",
            width: 640,
            height: 480,
            format: 0, // YUV420
            fps: 30,
            rotation: 0,
        }
    }
}
