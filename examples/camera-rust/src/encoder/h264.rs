use anyhow::Result;

// 导入 Encoder trait
use super::Encoder;

// 导入内存适配器和 NAL 解析器
use crate::memory::MemoryAdapter;
use crate::nal::{NALParser, NALUnitType};

// 导入绑定
include!(concat!(env!("OUT_DIR"), "/bindings.rs"));

pub struct H264Encoder {
    encoder: *mut VideoEncoder,
    width: u32,
    height: u32,
    input_buffers: Vec<VencInputBuffer>,
    memory_adapter: MemoryAdapter,
    force_keyframe: bool,
    sps_pps_data: Option<Vec<u8>>,
}

impl H264Encoder {
    pub fn new(width: u32, height: u32) -> Result<Self> {
        // 创建内存适配器
        let mut memory_adapter = MemoryAdapter::new()?;
        memory_adapter.open()?;

        unsafe {
            // 创建 H264 编码器
            let encoder = VideoEncCreate(VENC_CODEC_TYPE_VENC_CODEC_H264_VER2);
            if encoder.is_null() {
                anyhow::bail!("Failed to create H264 encoder");
            }

            // 配置编码器
            let mut base_config = VencBaseConfig {
                bEncH264Nalu: 1,
                nInputWidth: width,
                nInputHeight: height,
                nDstWidth: width,
                nDstHeight: height,
                nStride: width,
                eInputFormat: VENC_PIXEL_FMT_VENC_PIXEL_YVU420SP,
                memops: std::ptr::null_mut(),
                veOpsS: std::ptr::null_mut(),
                pVeOpsSelf: std::ptr::null_mut(),
                bOnlyWbFlag: 0,
                bLbcLossyComEnFlag2x: 0,
                bLbcLossyComEnFlag2_5x: 0,
                bIsVbvNoCache: 0,
            };

            // 配置 H264 参数
            let mut h264_param = VencH264Param {
                sProfileLevel: VencH264ProfileLevel {
                    nProfile: VENC_H264PROFILETYPE_VENC_H264ProfileMain,
                    nLevel: VENC_H264LEVELTYPE_VENC_H264Level31,
                },
                bEntropyCodingCABAC: 1,
                sQPRange: VencQPRange {
                    nMaxqp: 40,
                    nMinqp: 5,
                },
                nFramerate: 30,
                nSrcFramerate: 30,
                nBitrate: (width * height) as i32, // 码率设置为分辨率
                nMaxKeyInterval: 60 * 1000, // 60秒一个关键帧
                nCodingMode: VENC_CODING_MODE_VENC_FRAME_CODING,
                sGopParam: VencGopParam {
                    bUseGopCtrlEn: 0,
                    eGopMode: VENC_VIDEO_GOP_MODE_AW_NORMALP,
                    nVirtualIFrameInterval: 0,
                    nSpInterval: 0,
                    sRefParam: VencAdvancedRefParam {
                        bAdvancedRefEn: 0,
                        nBase: 0,
                        nEnhance: 0,
                        bRefBaseEn: 0,
                    },
                },
                sRcParam: VencRcParam {
                    eRcMode: VENC_RC_MODE_AW_VBR, // 采用动码率VBR
                    bUseSetMadThrdFlag: 0,
                    uMadThrdI: [0; 12],
                    uMadThrdP: [0; 12],
                    uMadThrdB: [0; 12],
                    uStatTime: 1,
                    uMinIQp: 5,
                    nMaxReEncodeTimes: 1,
                    sVbrParam: VencVbrParam {
                        uMaxBitRate: (width * height * 3) as u32, // vbr最大编码三倍正常码率
                        nMovingTh: 20,
                        nQuality: 9,
                    },
                    sFixQp: VencFixQP {
                        bEnable: 0,
                        nIQp: 20,
                        nPQp: 25,
                    },
                    sQpMap: VencMBModeCtrl {
                        mode_ctrl_en: 0,
                        p_info: std::ptr::null_mut(),
                    },
                    uRowQpDelta: 0,
                    uDirectionThrd: 0,
                    uQpDeltaLevelI: 0,
                    uQpDeltaLevelP: 0,
                    uQpDeltaLevelB: 0,
                    uInputFrmRate: 0,
                    uOutputFrmRate: 0,
                    uFluctuateLevel: 0,
                    uMinIprop: 0,
                    uMaxIprop: 0,
                },
            };

            // 设置 H264 参数
            let ret = VideoEncSetParameter(
                encoder,
                VENC_INDEXTYPE_VENC_IndexParamH264Param,
                &mut h264_param as *mut _ as *mut std::os::raw::c_void
            );
            if ret != VENC_RESULT_TYPE_VENC_RESULT_OK {
                VideoEncDestroy(encoder);
                anyhow::bail!("Failed to set H264 parameters: {}", ret);
            }

            // 禁用滤镜
            let value = 0;
            VideoEncSetParameter(encoder, VENC_INDEXTYPE_VENC_IndexParamIfilter, &value as *const _ as *mut std::os::raw::c_void);

            // 禁用旋转
            let value = 0;
            VideoEncSetParameter(encoder, VENC_INDEXTYPE_VENC_IndexParamRotation, &value as *const _ as *mut std::os::raw::c_void);

            // 禁用 P 帧跳过
            let value = 0;
            VideoEncSetParameter(encoder, VENC_INDEXTYPE_VENC_IndexParamSetPSkip, &value as *const _ as *mut std::os::raw::c_void);

            // 初始化编码器
            let ret = VideoEncInit(encoder, &mut base_config);
            if ret != VENC_RESULT_TYPE_VENC_RESULT_OK {
                VideoEncDestroy(encoder);
                anyhow::bail!("Failed to initialize encoder: {}", ret);
            }

            // 分配输入缓冲区
            let mut buffer_param = VencAllocateBufferParam {
                nBufferNum: 2,
                nSizeY: (width * height) as u32,
                nSizeC: (width * height / 2) as u32,
            };

            let ret = AllocInputBuffer(encoder, &mut buffer_param);
            if ret != VENC_RESULT_TYPE_VENC_RESULT_OK {
                VideoEncDestroy(encoder);
                anyhow::bail!("Failed to allocate input buffers: {}", ret);
            }

            Ok(Self {
                encoder,
                width,
                height,
                input_buffers: Vec::new(),
                memory_adapter,
                force_keyframe: false,
                sps_pps_data: None,
            })
        }
    }

    // 获取 SPS/PPS 数据
    pub fn get_sps_pps(&self) -> Result<Vec<u8>> {
        unsafe {
            let mut header_data = VencHeaderData {
                pBuffer: std::ptr::null_mut(),
                nLength: 0,
            };

            let ret = VideoEncGetParameter(
                self.encoder,
                VENC_INDEXTYPE_VENC_IndexParamH264SPSPPS,
                &mut header_data as *mut _ as *mut std::os::raw::c_void
            );

            if ret != VENC_RESULT_TYPE_VENC_RESULT_OK {
                anyhow::bail!("Failed to get SPS/PPS: {}", ret);
            }

            if header_data.pBuffer.is_null() || header_data.nLength == 0 {
                anyhow::bail!("Empty SPS/PPS data");
            }

            let data = std::slice::from_raw_parts(
                header_data.pBuffer,
                header_data.nLength as usize
            ).to_vec();

            Ok(data)
        }
    }

    // 强制编码关键帧
    pub fn force_keyframe(&mut self) -> Result<()> {
        self.force_keyframe = true;
        Ok(())
    }
}

impl super::Encoder for H264Encoder {
    fn encode(&mut self, data: &[u8]) -> Result<Vec<u8>> {
        unsafe {
            // 处理强制关键帧
            if self.force_keyframe {
                // 强制编码器编I帧
                let value = 1;
                VideoEncSetParameter(
                    self.encoder,
                    VENC_INDEXTYPE_VENC_IndexParamForceKeyFrame,
                    &value as *const _ as *mut std::os::raw::c_void
                );
                self.force_keyframe = false;

                // 获取 SPS/PPS 信息
                let mut sps_pps_data = VencHeaderData {
                    pBuffer: std::ptr::null_mut(),
                    nLength: 0,
                };

                let ret = VideoEncGetParameter(
                    self.encoder,
                    VENC_INDEXTYPE_VENC_IndexParamH264SPSPPS,
                    &mut sps_pps_data as *mut _ as *mut std::os::raw::c_void
                );

                if ret == VENC_RESULT_TYPE_VENC_RESULT_OK && !sps_pps_data.pBuffer.is_null() && sps_pps_data.nLength > 0 {
                    self.sps_pps_data = Some(std::slice::from_raw_parts(
                        sps_pps_data.pBuffer,
                        sps_pps_data.nLength as usize
                    ).to_vec());
                }
            }

            // 配置输入缓冲区
            let mut input_buffer = VencInputBuffer {
                nID: 0,
                nPts: 0,
                nFlag: 0,
                pAddrPhyY: std::ptr::null_mut(),
                pAddrPhyC: std::ptr::null_mut(),
                pAddrVirY: std::ptr::null_mut(),
                pAddrVirC: std::ptr::null_mut(),
                bEnableCorp: 0,
                sCropInfo: VencRect {
                    nLeft: 0,
                    nTop: 0,
                    nWidth: self.width as i32,
                    nHeight: self.height as i32,
                },
                ispPicVar: 0,
                ispPicVarChroma: 0,
                bUseInputBufferRoi: 0,
                roi_param: [VencROIConfig {
                    bEnable: 0,
                    index: 0,
                    nQPoffset: 0,
                    roi_abs_flag: 0,
                    sRect: VencRect {
                        nLeft: 0,
                        nTop: 0,
                        nWidth: 0,
                        nHeight: 0,
                    },
                }; 8],
                bAllocMemSelf: 0,
                nShareBufFd: 0,
                bUseCsiColorFormat: 0,
                eCsiColorFormat: VENC_PIXEL_FMT_VENC_PIXEL_YVU420SP,
                envLV: 0,
            };

            // 获取一个输入缓冲区
            let ret = GetOneAllocInputBuffer(self.encoder, &mut input_buffer);
            if ret != VENC_RESULT_TYPE_VENC_RESULT_OK {
                anyhow::bail!("Failed to get input buffer: {}", ret);
            }

            // 复制数据到缓冲区
            if !input_buffer.pAddrVirY.is_null() && !input_buffer.pAddrVirC.is_null() {
                let y_size = (self.width * self.height) as usize;
                let c_size = (self.width * self.height / 2) as usize;

                if data.len() >= y_size + c_size {
                    // 复制 Y 分量
                    std::ptr::copy_nonoverlapping(
                        data.as_ptr(),
                        input_buffer.pAddrVirY,
                        y_size
                    );
                    // 复制 C 分量
                    std::ptr::copy_nonoverlapping(
                        data.as_ptr().add(y_size),
                        input_buffer.pAddrVirC,
                        c_size
                    );
                } else {
                    ReturnOneAllocInputBuffer(self.encoder, &mut input_buffer);
                    anyhow::bail!("Input data too small");
                }
            }

            // 刷新缓存
            let ret = FlushCacheAllocInputBuffer(self.encoder, &mut input_buffer);
            if ret != VENC_RESULT_TYPE_VENC_RESULT_OK {
                ReturnOneAllocInputBuffer(self.encoder, &mut input_buffer);
                anyhow::bail!("Failed to flush cache: {}", ret);
            }

            // 添加输入缓冲区
            let ret = AddOneInputBuffer(self.encoder, &mut input_buffer);
            if ret != VENC_RESULT_TYPE_VENC_RESULT_OK {
                ReturnOneAllocInputBuffer(self.encoder, &mut input_buffer);
                anyhow::bail!("Failed to add input buffer: {}", ret);
            }

            // 编码一帧
            let ret = VideoEncodeOneFrame(self.encoder);
            if ret != VENC_RESULT_TYPE_VENC_RESULT_OK {
                ReturnOneAllocInputBuffer(self.encoder, &mut input_buffer);
                anyhow::bail!("Failed to encode frame: {}", ret);
            }

            // 标记输入缓冲区已使用
            AlreadyUsedInputBuffer(self.encoder, &mut input_buffer);

            // 返回输入缓冲区
            ReturnOneAllocInputBuffer(self.encoder, &mut input_buffer);

            // 获取编码输出
            let mut output_buffer = VencOutputBuffer {
                nID: 0,
                nPts: 0,
                nFlag: 0,
                nSize0: 0,
                nSize1: 0,
                pData0: std::ptr::null_mut(),
                pData1: std::ptr::null_mut(),
                frame_info: FrameInfo {
                    CurrQp: 0,
                    avQp: 0,
                    nGopIndex: 0,
                    nFrameIndex: 0,
                    nTotalIndex: 0,
                },
                nSize2: 0,
                pData2: std::ptr::null_mut(),
            };

            let mut encoded_data = Vec::new();

            // 如果有 SPS/PPS 数据，先添加
            if let Some(sps_pps) = &self.sps_pps_data {
                encoded_data.extend_from_slice(sps_pps);
            }

            // 循环获取所有输出帧
            loop {
                let ret = GetOneBitstreamFrame(self.encoder, &mut output_buffer);
                if ret != VENC_RESULT_TYPE_VENC_RESULT_OK {
                    break;
                }

                // 收集编码数据
                if !output_buffer.pData0.is_null() && output_buffer.nSize0 > 0 {
                    let data = std::slice::from_raw_parts(
                        output_buffer.pData0,
                        output_buffer.nSize0 as usize
                    );
                    encoded_data.extend_from_slice(data);
                }

                if !output_buffer.pData1.is_null() && output_buffer.nSize1 > 0 {
                    let data = std::slice::from_raw_parts(
                        output_buffer.pData1,
                        output_buffer.nSize1 as usize
                    );
                    encoded_data.extend_from_slice(data);
                }

                if !output_buffer.pData2.is_null() && output_buffer.nSize2 > 0 {
                    let data = std::slice::from_raw_parts(
                        output_buffer.pData2,
                        output_buffer.nSize2 as usize
                    );
                    encoded_data.extend_from_slice(data);
                }

                // 释放输出缓冲区
                FreeOneBitStreamFrame(self.encoder, &mut output_buffer);
            }

            Ok(encoded_data)
        }
    }
    
    fn flush(&mut self) -> Result<Vec<u8>> {
        unsafe {
            let mut encoded_data = Vec::new();
            let mut output_buffer = VencOutputBuffer {
                nID: 0,
                nPts: 0,
                nFlag: 0,
                nSize0: 0,
                nSize1: 0,
                pData0: std::ptr::null_mut(),
                pData1: std::ptr::null_mut(),
                frame_info: FrameInfo {
                    CurrQp: 0,
                    avQp: 0,
                    nGopIndex: 0,
                    nFrameIndex: 0,
                    nTotalIndex: 0,
                },
                nSize2: 0,
                pData2: std::ptr::null_mut(),
            };

            // 循环获取所有剩余的输出帧
            loop {
                let ret = GetOneBitstreamFrame(self.encoder, &mut output_buffer);
                if ret != VENC_RESULT_TYPE_VENC_RESULT_OK {
                    break;
                }

                // 收集编码数据
                if !output_buffer.pData0.is_null() && output_buffer.nSize0 > 0 {
                    let data = std::slice::from_raw_parts(
                        output_buffer.pData0,
                        output_buffer.nSize0 as usize
                    );
                    encoded_data.extend_from_slice(data);
                }

                if !output_buffer.pData1.is_null() && output_buffer.nSize1 > 0 {
                    let data = std::slice::from_raw_parts(
                        output_buffer.pData1,
                        output_buffer.nSize1 as usize
                    );
                    encoded_data.extend_from_slice(data);
                }

                if !output_buffer.pData2.is_null() && output_buffer.nSize2 > 0 {
                    let data = std::slice::from_raw_parts(
                        output_buffer.pData2,
                        output_buffer.nSize2 as usize
                    );
                    encoded_data.extend_from_slice(data);
                }

                // 释放输出缓冲区
                FreeOneBitStreamFrame(self.encoder, &mut output_buffer);
            }

            Ok(encoded_data)
        }
    }
    
    fn stop(&mut self) -> Result<()> {
        unsafe {
            // 释放输入缓冲区资源
            ReleaseAllocInputBuffer(self.encoder);

            // 反初始化编码器
            VideoEncUnInit(self.encoder);

            // 销毁编码器
            VideoEncDestroy(self.encoder);

            Ok(())
        }
    }
}

// 实现 Drop trait 确保资源被正确释放
impl Drop for H264Encoder {
    fn drop(&mut self) {
        let _ = self.stop();
    }
}
