use anyhow::Result;

#[derive(Debug, PartialEq)]
pub enum NALUnitType {
    NAL_UNKNOWN = 0,
    NAL_SLICE = 1,
    NAL_DPA = 2,
    NAL_DPB = 3,
    NAL_DPC = 4,
    NAL_IDR = 5,  // 关键帧
    NAL_SEI = 6,
    NAL_SPS = 7,  // 序列参数集
    NAL_PPS = 8,  // 图像参数集
    NAL_AUD = 9,
    NAL_FILLER = 12,
}

impl From<u8> for NALUnitType {
    fn from(value: u8) -> Self {
        match value & 0x1F {
            1 => NALUnitType::NAL_SLICE,
            2 => NALUnitType::NAL_DPA,
            3 => NALUnitType::NAL_DPB,
            4 => NALUnitType::NAL_DPC,
            5 => NALUnitType::NAL_IDR,
            6 => NALUnitType::NAL_SEI,
            7 => NALUnitType::NAL_SPS,
            8 => NALUnitType::NAL_PPS,
            9 => NALUnitType::NAL_AUD,
            12 => NALUnitType::NAL_FILLER,
            _ => NALUnitType::NAL_UNKNOWN,
        }
    }
}

pub struct NALParser;

impl NALParser {
    /// 判断 NAL 单元类型
    pub fn get_nal_type(data: &[u8]) -> NALUnitType {
        let data_len = data.len();
        let mut pos = 0;

        // 跳过起始码
        if data_len >= 4 && data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x00 && data[3] == 0x01 {
            pos = 4;
        } else if data_len >= 3 && data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x01 {
            pos = 3;
        } else {
            return NALUnitType::NAL_UNKNOWN;
        }

        if pos < data_len {
            NALUnitType::from(data[pos])
        } else {
            NALUnitType::NAL_UNKNOWN
        }
    }

    /// 获取一个 H.264 NALU
    pub fn get_h264_nalu(buffer: &[u8]) -> Result<usize> {
        let length = buffer.len();
        let mut pos = 0;

        // 先取出第一个起始码 0x00000001
        while pos < length - 1 && buffer[pos] == 0 {
            pos += 1;
        }

        if pos >= length || buffer[pos] != 1 || pos < 3 {
            anyhow::bail!("Invalid NAL unit start code");
        }

        pos += 1;

        // 连续读取码流直到出现下一个起始码 0x00000001
        while pos < length - 3 {
            if buffer[pos-3] == 0 && buffer[pos-2] == 0 && buffer[pos-1] == 0 && buffer[pos] == 1 {
                return Ok(pos - 3);
            }
            pos += 1;
        }

        // 如果没有找到下一个起始码，返回整个缓冲区长度
        Ok(length)
    }

    /// 检查是否是 SPS 数据
    pub fn is_sps(data: &[u8]) -> bool {
        Self::get_nal_type(data) == NALUnitType::NAL_SPS
    }

    /// 检查是否是 PPS 数据
    pub fn is_pps(data: &[u8]) -> bool {
        Self::get_nal_type(data) == NALUnitType::NAL_PPS
    }

    /// 检查是否是 IDR 帧（关键帧）
    pub fn is_idr(data: &[u8]) -> bool {
        Self::get_nal_type(data) == NALUnitType::NAL_IDR
    }
}
