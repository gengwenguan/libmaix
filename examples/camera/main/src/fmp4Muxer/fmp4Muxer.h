/*********************************************************************************
  *Copyright(C),Your Company
  *FileName:  fmp4Muxer.h
  *Author:    gengwenguan
  *Date:      2026-05-23
  *Description:  fMP4 (Fragmented MP4 / ISOBMFF) 封装器
                 输入 H264 Annex-B + AAC raw（带 PTS），输出 MSE 友好的字节流
                 - 第一段：ftyp + moov（init segment，仅一次）
                 - 后续：moof + mdat（每个 keyframe 切一段）
**********************************************************************************/
#pragma once
#include <memory>
#include <vector>
#include <mutex>
#include <cstdint>

extern "C" {
    #include <libavformat/avformat.h>
    #include <libavcodec/avcodec.h>
    #include <libavutil/avutil.h>
}

class C_Fmp4Muxer
{
public:
    class C_Listener {
    public:
        virtual ~C_Listener() = default;
        // ftyp+moov 写完时回调一次（init segment）
        virtual void OnInitSegment(const uint8_t* data, size_t len) = 0;
        // 每写完一个完整 fragment（moof+mdat）回调
        virtual void OnFragment(const uint8_t* data, size_t len) = 0;
    };

    struct VideoConfig {
        int                    width;
        int                    height;
        const unsigned char*   spsPpsAnnexB;   // Annex-B 格式的 SPS+PPS
        unsigned int           spsPpsLen;
    };
    struct AudioConfig {
        int                    sampleRate;
        int                    channels;
        const unsigned char*   asc;            // AudioSpecificConfig（2 字节）
        unsigned int           ascLen;
    };

public:
    C_Fmp4Muxer(C_Listener* pListener,
                const VideoConfig& v,
                const AudioConfig& a);
    ~C_Fmp4Muxer();

    // 输入一帧 H264（Annex-B），ptsUs 单位微秒，isKey=true 表示包含 IDR
    // 内部会做 Annex-B → AVCC 的转换
    int InputH264(const uint8_t* data, unsigned int dataLen,
                  int64_t ptsUs, bool isKey);

    // 输入一帧 AAC raw（无 ADTS 头），ptsUs 单位微秒
    int InputAac(const uint8_t* data, unsigned int dataLen,
                 int64_t ptsUs);

    bool IsReady() const { return m_bReady; }

private:
    // 将 Annex-B 的多 NAL 数据转成 AVCC 格式（4字节大端长度前缀 + nal_payload）
    // 返回转换后字节数
    static unsigned int AnnexBToAvcc(const uint8_t* in, unsigned int inLen,
                                     std::vector<uint8_t>& out);
    // 从 SPS/PPS（Annex-B）构造 avcC box，写入 video stream extradata
    bool BuildAvcC(const unsigned char* spsPps, unsigned int len,
                   std::vector<uint8_t>& avcC);

    // AVIO 写回调：muxer 输出的所有字节都流到这里
    static int  OnAvioWrite(void* opaque, uint8_t* buf, int bufSize);
    void        FlushBufferAsFragment();

private:
    C_Listener*       m_pListener;
    bool              m_bReady = false;
    bool              m_bInitEmitted = false;

    AVFormatContext*  m_fmtCtx = nullptr;
    AVStream*         m_vStream = nullptr;
    AVStream*         m_aStream = nullptr;
    AVIOContext*      m_ioCtx = nullptr;
    uint8_t*          m_ioBuf = nullptr;          // avio 内部缓冲，所有权在 ioCtx

    // 累积 buffer：muxer 写到这里，达到一个 fragment 边界再吐出
    std::vector<uint8_t> m_accumBuf;
    std::mutex           m_muxMu;                 // 保护 m_fmtCtx + m_accumBuf

    // 临时 AVCC 输出缓冲，避免每帧 alloc
    std::vector<uint8_t> m_avccBuf;

    // 视频上一帧 PTS（us），用于估算 duration（当前帧 - 上一帧）
    int64_t              m_lastVideoPtsUs = AV_NOPTS_VALUE;
    // 音频采样率（用于 AAC 时基换算）
    int                  m_audioSampleRate = 48000;
    // 首个进入 muxer 的 PTS（us），所有后续 PTS 都减去它使得首帧从 0 开始
    // 否则 mov muxer 的 interleave 预滚会把某个流挪到负 baseMediaDecodeTime
    // 触发 "Packets poorly interleaved, failed to avoid negative timestamp"
    int64_t              m_firstPtsUs = AV_NOPTS_VALUE;

    // mov/mp4 内部对超大时间戳支持差，这里用各自标准时基
    static constexpr AVRational kTbVideo90k = AVRational{1, 90000};
};
