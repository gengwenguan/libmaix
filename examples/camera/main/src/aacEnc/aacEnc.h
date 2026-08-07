/*********************************************************************************
  *Copyright(C),Your Company
  *FileName:  aacEnc.h
  *Author:    gengwenguan
  *Date:      2026-05-23
  *Description:  音频采集编码为AAC-LC格式，单通道、48kHz采样、64kbps码率
                 用于fMP4标准容器封装，相比Opus兼容性更好（Safari/iOS可播）
**********************************************************************************/
#pragma once
#include <memory>
#include <iostream>
#include <thread>
#include <mutex>
#include <atomic>
#include <cstdint>
#include <cstdio>

extern "C" {
    #include <alsa/asoundlib.h>
    #include <libavutil/avutil.h>
    #include <libavutil/audio_fifo.h>
    #include <libavutil/opt.h>
    #include <libavdevice/avdevice.h>
    #include <libavformat/avformat.h>
    #include <libavcodec/avcodec.h>
    #include <libswresample/swresample.h>
}

// 麦克风实时响度（0~100，低响度区放大后封顶）。由 AAC 采集线程每 20ms 更新一次，
// 供补光灯声控触发等模块跨线程无锁读取；未在采集时返回 0。
// 放全局函数而非 C_AacEnc 成员，是为了让 lightController 不必持有编码器实例。
int AacEnc_GetMicLoudness();

class C_AacEnc
{
private:
    static constexpr unsigned int kSampleRate   = 48000;
    static constexpr unsigned int kChannels     = 1;
    static constexpr unsigned long kAlsaPeriod  = 960;   // ALSA 一次读 20ms
    static constexpr unsigned int kAacFrameSize = 1024;  // AAC 固定 1024 samples/frame
    static constexpr unsigned int kBitRate      = 64000; // 64 kbps
    static constexpr unsigned int kFifoCapacity = 8192;  // FLTP 缓冲容量(samples)
public:
    class C_Listener {
    public:
        virtual ~C_Listener() = default;
        // 回调输出 raw AAC（无 ADTS 头），由 fMP4 muxer 自行添加 ASC
        // ptsUs 为该帧首样本的时间戳，单位微秒
        virtual int OnOutputAac(unsigned char* data, unsigned int dataLen,
                                int64_t ptsUs) = 0;
    };
    struct Biquad {
        double b0 = 1.0, b1 = 0.0, b2 = 0.0;
        double a1 = 0.0, a2 = 0.0;
        double z1 = 0.0, z2 = 0.0;
        double Process(double x);
        void Reset();
    };
public:
    C_AacEnc(C_Listener* pListener);
    ~C_AacEnc();

    // 构造函数只初始化 ALSA/FFmpeg 资源，不启动线程。由宿主在所有回调依赖
    // 都构造完成后显式 Start，退出时先 Stop 再销毁 listener。
    int Start();
    void Stop();

    // AAC-LC 的 AudioSpecificConfig（用于 fMP4 esds box）
    // 返回长度，data 由内部维护，调用方仅读取
    const unsigned char* GetAudioSpecificConfig(unsigned int* outLen) const;

    unsigned int SampleRate() const { return kSampleRate; }
    unsigned int Channels()   const { return kChannels;   }

private:
    // 音频采集编码线程
    void CaptureEncoder();
    // 编码并输出一个 1024 sample 的 AAC 帧
    void EncodeOneFrame();
    // 构造 AudioSpecificConfig（2字节）
    void BuildAudioSpecificConfig();
    // 按配置页选择的 preset，对原始 S16 PCM 做轻量实时滤波。
    void ApplyMicFilter(short* samples, unsigned int frames);

private:
    C_Listener*       m_pListener;
    snd_pcm_t*        m_capture_handle = nullptr;
    AVCodecContext*   m_codec_ctx      = nullptr;
    SwrContext*       m_swr            = nullptr;
    AVAudioFifo*      m_fifo           = nullptr;
    int64_t           m_nextPts        = 0;     // 单位 sample 数

    unsigned char     m_asc[2]         = {0};   // AudioSpecificConfig
    unsigned int      m_ascLen         = 0;

    std::atomic<bool> m_bRun{false};
    std::unique_ptr<std::thread> m_pCaptureEncoderThread;

    // 调试用：若环境变量 AAC_DUMP_PATH 被设置，则以 ADTS 头方式 dump 到该文件
    // 本地拉回后可以直接 ffplay 听效果，用于验证 ALSA 采集 + AAC 编码质量
    FILE*             m_pDumpFile      = nullptr;
    void WriteAdtsAndDump(const unsigned char* aac, unsigned int aacLen);

    // 原始麦克风 PCM dump：若环境变量 MIC_PCM_DUMP_PATH 被设置，则把 ALSA 刚采集到
    // 的 S16_LE/mono/48kHz 数据写成 WAV，便于离线频谱分析电流声/底噪。
    FILE*             m_pPcmDumpFile   = nullptr;
    uint32_t          m_pcmDumpBytes   = 0;
    uint32_t          m_pcmDumpMaxBytes = 0;
    void InitPcmDump();
    void WritePcmDump(const short* samples, unsigned int frames);
    void ClosePcmDump();

    Biquad            m_filterChain[16];
    int               m_filterCount = 0;
    int               m_filterMode  = -1;
    double            m_gateGain    = 1.0;
    void RebuildMicFilter(int mode);
};
