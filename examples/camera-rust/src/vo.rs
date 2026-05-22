use anyhow::Result;

// 导入绑定
include!(concat!(env!("OUT_DIR"), "/bindings.rs"));

pub struct VideoOutput {
    vo: *mut libmaix_vo_t,
}

impl VideoOutput {
    pub fn new() -> Result<Self> {
        // 输入分辨率 640x480，输出分辨率 240x240（M2dock显示屏固定分辨率）
        let vo = unsafe {
            libmaix_vo_create(640, 480, 0, 0, 240, 240)
        };
        
        if vo.is_null() {
            anyhow::bail!("Failed to create video output");
        }
        
        Ok(Self { vo })
    }
    
    pub fn init(&self, _config: &VOConfig) -> Result<()> {
        // libmaix_vo_create已经初始化了视频输出
        Ok(())
    }
    
    pub fn start(&self) -> Result<()> {
        // 视频输出在创建时已经启动
        Ok(())
    }
    
    pub fn stop(&self) -> Result<()> {
        // 视频输出在销毁时会自动停止
        Ok(())
    }
    
    pub fn get_frame(&self) -> Result<*mut ::std::os::raw::c_void> {
        unsafe {
            let get_frame = (*self.vo).get_frame;
            if let Some(get_frame) = get_frame {
                let frame = get_frame(self.vo, 0);
                if frame.is_null() {
                    anyhow::bail!("Failed to get frame");
                }
                Ok(frame)
            } else {
                anyhow::bail!("get_frame method not available");
            }
        }
    }
    
    pub fn frame_addr(&self, frame: *mut ::std::os::raw::c_void) -> Result<(*mut ::std::os::raw::c_void, *mut ::std::os::raw::c_void)> {
        unsafe {
            let frame_addr = (*self.vo).frame_addr;
            if let Some(frame_addr) = frame_addr {
                let mut vir: *mut u32 = std::ptr::null_mut();
                let mut phy: *mut u32 = std::ptr::null_mut();
                frame_addr(self.vo, frame, &mut vir, &mut phy);
                if vir.is_null() {
                    anyhow::bail!("Failed to get frame address");
                }
                Ok((vir as *mut ::std::os::raw::c_void, phy as *mut ::std::os::raw::c_void))
            } else {
                anyhow::bail!("frame_addr method not available");
            }
        }
    }
    
    pub fn set_frame(&self, frame: *mut ::std::os::raw::c_void) -> Result<()> {
        unsafe {
            let set_frame = (*self.vo).set_frame;
            if let Some(set_frame) = set_frame {
                let ret = set_frame(self.vo, frame, 0);
                if ret != libmaix_err_t_LIBMAIX_ERR_NONE {
                    anyhow::bail!("Failed to set frame: {}", ret);
                }
            } else {
                anyhow::bail!("set_frame method not available");
            }
        }
        Ok(())
    }
}

impl Drop for VideoOutput {
    fn drop(&mut self) {
        unsafe {
            libmaix_vo_destroy(&mut self.vo);
        }
    }
}

// 为VideoOutput实现Send和Sync trait，因为它包含原始指针
unsafe impl Send for VideoOutput {}
unsafe impl Sync for VideoOutput {}

pub struct VOConfig {
    pub width: u32,
    pub height: u32,
    pub format: u32,
    pub rotation: u32,
}

impl Default for VOConfig {
    fn default() -> Self {
        Self {
            width: 640,
            height: 480,
            format: 0, // YUV420
            rotation: 0,
        }
    }
}
