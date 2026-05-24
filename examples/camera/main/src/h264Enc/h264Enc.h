/********************************************************************************* 
  *Copyright(C),Your Company 
  *FileName:  h264Enc.h
  *Author:    gengwenguan
  *Date:      2024-10-25
  *Description:  h264编码类，可将输入的NV21格式YUV数据编码为H264数据
                 提供强制编码关键帧接口
**********************************************************************************/ 
#pragma once
#include<memory>
#include<iostream>
#include<vector>
#include "vencoder.h"

class C_H264Enc
{
public:
    // 定义NAL单元类型的枚举
    enum NALUnitType {
        NAL_UNKNOWN = 0,
        NAL_SLICE = 1,       //P帧或B帧
        NAL_IDR_PICTURE = 5, //I帧
        NAL_SEI = 6,
        NAL_SPS = 7,
        NAL_PPS = 8
        // 其他NAL单元类型可以根据需要继续添加
    };
public:
    class C_Listener{
    public:
        virtual ~C_Listener() = default;
        // 编码器输出一个完整的视频访问单元（access unit），可能含 SPS/PPS/IDR 多个 NAL
        // data 仍为 Annex-B 格式（startcode 分隔），由下游 muxer 自行转 AVCC
        // ptsUs : 该帧时间戳，单位微秒，与 AAC 共用 C_TimeBase
        // isKey : 是否含 IDR（即关键帧），fMP4 fragment 切片用
        virtual int OnOutputH264(unsigned char* data, unsigned int dataLen,
                                 int64_t ptsUs, bool isKey) = 0;
    };
public:
    C_H264Enc(C_Listener* pListener, unsigned int srcWight, unsigned int srcHight, unsigned int dstWidth, unsigned int dstHeight);
    ~C_H264Enc();
    //输入NV21采集数据
    int InputData(unsigned char* inputData);
    //强制编码一帧关键帧，此处进行标记，实际在送数据时进行控制强制I帧
    void ForceIframe(){
        std::cout << "ForceIframe()" << std::endl;
        m_forceIframe = true;
    }

    // 获取启动时缓存好的 SPS+PPS（Annex-B 格式，含 0x00000001 startcode）
    // muxer 启动时调用，用于填 video stream 的 extradata
    // 返回 nullptr 表示尚未生成（应在第一次编码后才有效）
    const unsigned char* GetSpsPps(unsigned int* outLen) const {
        if (m_spsPpsCache.empty()) { if (outLen) *outLen = 0; return nullptr; }
        if (outLen) *outLen = (unsigned int)m_spsPpsCache.size();
        return m_spsPpsCache.data();
    }

    // 判断NAL单元类型
    static NALUnitType GetNALType(unsigned char* data, unsigned int dataLen);

    /******************************************************************************
    * 功  能：获取一个h.264 nalu
    * 参  数：buffer - 目标缓冲区
    * 返回值：返回错误码或nalu长度
    ******************************************************************************/
    static int get_h264_nalu(unsigned char  *buffer, unsigned length);

private:
    // 缓存 SPS/PPS（Annex-B 形式）便于 muxer 拿
    void CacheSpsPpsFromVenc();
    // 扫描一段 Annex-B 数据是否包含 IDR NAL
    static bool ContainsIdr(const unsigned char* data, unsigned int len);

private:
    C_Listener* m_pListener;
    VideoEncoder *m_pVideoEnc;       //视频编码器
    VencH264Param m_h264Param;
    VencBaseConfig m_baseConfig;
    VencAllocateBufferParam m_bufferParam;
    VencInputBuffer m_inputBuffer;
    VencOutputBuffer m_outputBuffer;
    VencHeaderData m_sps_pps_data;
    bool           m_forceIframe = true;

    // 周期性强制 IDR 计数：硬编码器 nMaxKeyInterval 在 V83x 平台不可靠
    // 由我们每 30 帧（≈1s @30fps）主动 ForceIframe 一次
    uint64_t                   m_inputCount = 0;
    static constexpr uint64_t  kIdrIntervalFrames = 30;

    // SPS+PPS 缓存（Annex-B），首次编码后填充
    std::vector<unsigned char> m_spsPpsCache;
    // 临时聚合一帧多次输出（pData0 + pData1）
    std::vector<unsigned char> m_frameAssemble;
};