/*********************************************************************************
  *Copyright(C),Your Company
  *FileName:  aacEnc.cpp
  *Author:    gengwenguan
  *Date:      2026-05-23
  *Description:  AAC-LC 编码器实现，参考 opusEnc.cpp 结构
                 关键差异：
                   1) FFmpeg 原生 AAC 编码器需要 FLTP planar float 输入，
                      所以多了 swr_convert(S16 → FLTP) 一步；
                   2) AAC 帧长固定 1024 samples，与 ALSA 周期 960 不对齐，
                      用 AVAudioFifo 缓冲攒够再编码。
**********************************************************************************/
#include "aacEnc.h"
#include "appConfig.h"
#include "logAdapt.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cmath>

#define PRINT_ERROR(errnum) do { \
    char errbuf[AV_ERROR_MAX_STRING_SIZE]; \
    av_strerror(errnum, errbuf, sizeof(errbuf)); \
    CLOG_ERR("AAC ffmpeg error: %s (%d)\n", errbuf, errnum); \
} while (0)

namespace {
static constexpr double kPi = 3.14159265358979323846;

static void WriteLe16(FILE* fp, uint16_t v) {
    fputc((int)(v & 0xFF), fp);
    fputc((int)((v >> 8) & 0xFF), fp);
}

static void WriteLe32(FILE* fp, uint32_t v) {
    fputc((int)(v & 0xFF), fp);
    fputc((int)((v >> 8) & 0xFF), fp);
    fputc((int)((v >> 16) & 0xFF), fp);
    fputc((int)((v >> 24) & 0xFF), fp);
}

static short ClampS16(double v) {
    if (v > 32767.0) return 32767;
    if (v < -32768.0) return -32768;
    return (short)std::lrint(v);
}

static C_AacEnc::Biquad MakeHighpass(double freq, double q) {
    const double w0 = 2.0 * kPi * freq / 48000.0;
    const double cw = std::cos(w0);
    const double sw = std::sin(w0);
    const double alpha = sw / (2.0 * q);
    const double a0 = 1.0 + alpha;

    C_AacEnc::Biquad b;
    b.b0 =  (1.0 + cw) * 0.5 / a0;
    b.b1 = -(1.0 + cw)       / a0;
    b.b2 =  (1.0 + cw) * 0.5 / a0;
    b.a1 =  -2.0 * cw        / a0;
    b.a2 =  (1.0 - alpha)    / a0;
    return b;
}

static C_AacEnc::Biquad MakePeakingEq(double freq, double q, double gainDb) {
    const double a = std::pow(10.0, gainDb / 40.0);
    const double w0 = 2.0 * kPi * freq / 48000.0;
    const double cw = std::cos(w0);
    const double sw = std::sin(w0);
    const double alpha = sw / (2.0 * q);
    const double a0 = 1.0 + alpha / a;

    C_AacEnc::Biquad b;
    b.b0 = (1.0 + alpha * a) / a0;
    b.b1 = (-2.0 * cw)       / a0;
    b.b2 = (1.0 - alpha * a) / a0;
    b.a1 = (-2.0 * cw)       / a0;
    b.a2 = (1.0 - alpha / a) / a0;
    return b;
}
} // namespace

static void check_alsa_error(int err, const char *msg) {
    if (err < 0) {
        fprintf(stderr, "ALSA error: %s: %s\n", msg, snd_strerror(err));
        exit(EXIT_FAILURE);
    }
}

C_AacEnc::C_AacEnc(C_Listener* pListener)
    : m_pListener(pListener)
{
    int err;
    snd_pcm_hw_params_t* hw_params = nullptr;

    // 1. 打开 ALSA 采集设备
    err = snd_pcm_open(&m_capture_handle, "default", SND_PCM_STREAM_CAPTURE, 0);
    check_alsa_error(err, "Opening PCM device for capture");

    err = snd_pcm_hw_params_malloc(&hw_params);
    check_alsa_error(err, "Allocating hardware parameters");
    err = snd_pcm_hw_params_any(m_capture_handle, hw_params);
    check_alsa_error(err, "Initializing hardware parameters");
    err = snd_pcm_hw_params_set_access(m_capture_handle, hw_params,
                                       SND_PCM_ACCESS_RW_INTERLEAVED);
    check_alsa_error(err, "Setting access type");
    err = snd_pcm_hw_params_set_format(m_capture_handle, hw_params,
                                       SND_PCM_FORMAT_S16_LE);
    check_alsa_error(err, "Setting sample format");
    unsigned int rate = kSampleRate;
    err = snd_pcm_hw_params_set_rate_near(m_capture_handle, hw_params, &rate, 0);
    check_alsa_error(err, "Setting sample rate");
    err = snd_pcm_hw_params_set_channels(m_capture_handle, hw_params, kChannels);
    check_alsa_error(err, "Setting channel count");
    unsigned long period_size = kAlsaPeriod;
    err = snd_pcm_hw_params_set_period_size_near(m_capture_handle, hw_params,
                                                 &period_size, 0);
    check_alsa_error(err, "Setting period size");
    err = snd_pcm_hw_params(m_capture_handle, hw_params);
    check_alsa_error(err, "Setting hardware parameters");
    snd_pcm_hw_params_free(hw_params);

    err = snd_pcm_prepare(m_capture_handle);
    check_alsa_error(err, "Preparing the PCM device");

    // 2. 找到 AAC 编码器（FFmpeg 原生 aac，开箱即用）
    AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!codec) {
        CLOG_ERR("Could not find AV_CODEC_ID_AAC encoder\n");
        return;
    }

    m_codec_ctx = avcodec_alloc_context3(codec);
    if (!m_codec_ctx) {
        CLOG_ERR("Could not alloc AAC codec context\n");
        return;
    }

    m_codec_ctx->sample_rate    = kSampleRate;
    m_codec_ctx->channels       = kChannels;
    m_codec_ctx->channel_layout = AV_CH_LAYOUT_MONO;
    m_codec_ctx->sample_fmt     = AV_SAMPLE_FMT_FLTP;     // AAC 必须 FLTP
    m_codec_ctx->bit_rate       = kBitRate;
    m_codec_ctx->profile        = FF_PROFILE_AAC_LOW;     // AAC-LC
    m_codec_ctx->time_base      = AVRational{1, (int)kSampleRate};

    if (avcodec_open2(m_codec_ctx, codec, nullptr) < 0) {
        CLOG_ERR("Could not open AAC codec\n");
        return;
    }

    // 3. 重采样器：S16 interleaved → FLTP
    m_swr = swr_alloc();
    av_opt_set_int       (m_swr, "in_channel_layout",  AV_CH_LAYOUT_MONO,  0);
    av_opt_set_int       (m_swr, "out_channel_layout", AV_CH_LAYOUT_MONO,  0);
    av_opt_set_int       (m_swr, "in_sample_rate",     kSampleRate,        0);
    av_opt_set_int       (m_swr, "out_sample_rate",    kSampleRate,        0);
    av_opt_set_sample_fmt(m_swr, "in_sample_fmt",      AV_SAMPLE_FMT_S16,  0);
    av_opt_set_sample_fmt(m_swr, "out_sample_fmt",     AV_SAMPLE_FMT_FLTP, 0);
    if (swr_init(m_swr) < 0) {
        CLOG_ERR("Could not init swr context\n");
        return;
    }

    // 4. AAC 帧 1024 samples，需要 FIFO 攒数据
    m_fifo = av_audio_fifo_alloc(AV_SAMPLE_FMT_FLTP, kChannels, kFifoCapacity);
    if (!m_fifo) {
        CLOG_ERR("Could not alloc audio fifo\n");
        return;
    }

    // 5. 构造 AudioSpecificConfig（fMP4 esds 用）
    BuildAudioSpecificConfig();

    // 6. 调试 dump：若设置了 AAC_DUMP_PATH 环境变量，则把每帧带 ADTS 头写到该文件
    //    用于独立验证 ALSA 采集 + AAC 编码音质（ffplay 可直接播放）
    const char* dumpPath = getenv("AAC_DUMP_PATH");
    if (dumpPath && *dumpPath) {
        m_pDumpFile = fopen(dumpPath, "wb");
        if (m_pDumpFile) {
            CLOG_INF("AAC dump enabled (with ADTS): %s\n", dumpPath);
        } else {
            CLOG_ERR("AAC dump open failed: %s\n", dumpPath);
        }
    }
    InitPcmDump();

    m_bRun = true;
    m_pCaptureEncoderThread = std::unique_ptr<std::thread>(
        new std::thread([this]() { this->CaptureEncoder(); })
    );
    CLOG_INF("AAC encoder started: %uHz %uch %ukbps\n",
             kSampleRate, kChannels, kBitRate / 1000);
}

C_AacEnc::~C_AacEnc()
{
    m_bRun = false;
    if (m_pCaptureEncoderThread && m_pCaptureEncoderThread->joinable()) {
        m_pCaptureEncoderThread->join();
    }

    if (m_fifo)        { av_audio_fifo_free(m_fifo); m_fifo = nullptr; }
    if (m_swr)         { swr_free(&m_swr); }
    if (m_codec_ctx)   { avcodec_close(m_codec_ctx); avcodec_free_context(&m_codec_ctx); }
    if (m_capture_handle) { snd_pcm_close(m_capture_handle); m_capture_handle = nullptr; }
    if (m_pDumpFile)   { fclose(m_pDumpFile); m_pDumpFile = nullptr; }
    ClosePcmDump();

    CLOG_INF("~C_AacEnc end\n");
}

const unsigned char* C_AacEnc::GetAudioSpecificConfig(unsigned int* outLen) const
{
    if (outLen) *outLen = m_ascLen;
    return m_asc;
}

double C_AacEnc::Biquad::Process(double x)
{
    const double y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
}

void C_AacEnc::Biquad::Reset()
{
    z1 = 0.0;
    z2 = 0.0;
}

void C_AacEnc::RebuildMicFilter(int mode)
{
    m_filterCount = 0;
    m_filterMode = mode;
    m_gateGain = 1.0;

    auto add = [this](const Biquad& b) {
        if (m_filterCount < (int)(sizeof(m_filterChain) / sizeof(m_filterChain[0]))) {
            m_filterChain[m_filterCount++] = b;
        }
    };
    auto eq = [](double f, double q, double g) {
        return MakePeakingEq(f, q, g);
    };

    switch (mode) {
    case 1: // 均衡推荐：hp120 + 主要电流声谐波抑制
    case 5: // 均衡 + 噪声门：滤波参数同 mode 1
        add(MakeHighpass(120.0, 0.707));
        add(eq(50.0,  10.0, -30.0));
        add(eq(100.0, 12.0, -24.0));
        add(eq(150.0, 12.0, -18.0));
        add(eq(200.0, 14.0, -12.0));
        add(eq(250.0, 16.0, -10.0));
        add(eq(300.0, 18.0,  -8.0));
        add(eq(550.0, 20.0, -10.0));
        break;
    case 2: // 激进：更强低频切除，声音会更薄
        add(MakeHighpass(150.0, 0.707));
        add(eq(50.0,  10.0, -36.0));
        add(eq(100.0, 12.0, -30.0));
        add(eq(150.0, 12.0, -24.0));
        add(eq(200.0, 14.0, -18.0));
        add(eq(250.0, 16.0, -14.0));
        add(eq(300.0, 18.0, -12.0));
        add(eq(350.0, 18.0,  -8.0));
        add(eq(550.0, 20.0, -12.0));
        add(eq(650.0, 22.0,  -8.0));
        add(eq(750.0, 22.0,  -8.0));
        break;
    case 3: // 精细：压制 300~800Hz 残留峰，适合追求更低底噪
        add(MakeHighpass(150.0, 0.707));
        add(eq(50.0,  10.0, -36.0));
        add(eq(100.0, 12.0, -30.0));
        add(eq(150.0, 12.0, -24.0));
        add(eq(200.0, 14.0, -18.0));
        add(eq(250.0, 16.0, -14.0));
        add(eq(300.0, 18.0, -12.0));
        add(eq(350.0, 22.0, -10.0));
        add(eq(400.0, 22.0, -10.0));
        add(eq(450.0, 22.0, -10.0));
        add(eq(550.0, 24.0, -14.0));
        add(eq(650.0, 24.0, -10.0));
        add(eq(750.0, 24.0, -10.0));
        break;
    case 4: // 极激进：静音最干净，但男声低频损失最大
        add(MakeHighpass(180.0, 0.707));
        add(eq(50.0,  10.0, -40.0));
        add(eq(100.0, 12.0, -36.0));
        add(eq(150.0, 12.0, -30.0));
        add(eq(200.0, 14.0, -24.0));
        add(eq(250.0, 16.0, -18.0));
        add(eq(300.0, 18.0, -14.0));
        add(eq(350.0, 18.0, -10.0));
        add(eq(550.0, 20.0, -12.0));
        break;
    default:
        break;
    }

    CLOG_INF("MIC filter mode=%d filters=%d\n", mode, m_filterCount);
}

void C_AacEnc::ApplyMicFilter(short* samples, unsigned int frames)
{
    if (!samples || frames == 0) return;

    const int mode = C_AppConfig::GetInst().GetSnapshot().mic_filter_mode;
    if (mode != m_filterMode) {
        RebuildMicFilter(mode);
    }
    if (mode <= 0 || m_filterCount <= 0) return;

    double sum2 = 0.0;
    for (unsigned int i = 0; i < frames; ++i) {
        double x = (double)samples[i];
        for (int j = 0; j < m_filterCount; ++j) {
            x = m_filterChain[j].Process(x);
        }
        if (mode == 5) {
            sum2 += x * x;
        }
        samples[i] = ClampS16(x);
    }

    if (mode == 5) {
        // 软噪声门/扩展器：不要在阈值处 0.12 <-> 1.0 硬切，否则说话起止
        // 会有明显“开关门”听感。这里让增益随 RMS 连续变化，并慢速关门。
        const double rms = std::sqrt(sum2 / std::max(1u, frames));

        // close/open 之间用 smoothstep 过渡：低电平只压到 0.28，不再压到 0.12。
        // 这样静音略少一点，但说话后的尾音和环境底噪衔接更自然。
        const double closeRms = 180.0;  // 约 -45dBFS
        const double openRms  = 700.0;  // 约 -33dBFS
        double u = (rms - closeRms) / (openRms - closeRms);
        if (u < 0.0) u = 0.0;
        if (u > 1.0) u = 1.0;
        u = u * u * (3.0 - 2.0 * u);

        const double minGain = 0.28;
        const double target = minGain + (1.0 - minGain) * u;

        // 打开要跟得上人声，关闭要慢，避免字间/句尾抽吸。
        const double alpha  = (target > m_gateGain) ? 0.22 : 0.018;
        m_gateGain += (target - m_gateGain) * alpha;
        for (unsigned int i = 0; i < frames; ++i) {
            samples[i] = ClampS16((double)samples[i] * m_gateGain);
        }
    }
}

void C_AacEnc::InitPcmDump()
{
    const char* dumpPath = getenv("MIC_PCM_DUMP_PATH");
    if (!dumpPath || !*dumpPath) return;

    int dumpSec = 60; // 默认只抓 60 秒，避免长期写盘
    const char* dumpSecEnv = getenv("MIC_PCM_DUMP_SEC");
    if (dumpSecEnv && *dumpSecEnv) {
        int v = atoi(dumpSecEnv);
        if (v > 0) dumpSec = v;
    }
    const uint64_t maxBytes =
        (uint64_t)dumpSec * kSampleRate * kChannels * sizeof(int16_t);
    m_pcmDumpMaxBytes = (maxBytes > 0xFFFFFFFFu) ? 0xFFFFFFFFu : (uint32_t)maxBytes;
    m_pcmDumpBytes = 0;

    m_pPcmDumpFile = fopen(dumpPath, "wb");
    if (!m_pPcmDumpFile) {
        CLOG_ERR("MIC PCM dump open failed: %s\n", dumpPath);
        return;
    }

    // 先写占位 WAV 头，ClosePcmDump() 时回填 RIFF/data 长度。
    fwrite("RIFF", 1, 4, m_pPcmDumpFile);
    WriteLe32(m_pPcmDumpFile, 36);
    fwrite("WAVE", 1, 4, m_pPcmDumpFile);
    fwrite("fmt ", 1, 4, m_pPcmDumpFile);
    WriteLe32(m_pPcmDumpFile, 16);                       // PCM fmt chunk size
    WriteLe16(m_pPcmDumpFile, 1);                        // PCM
    WriteLe16(m_pPcmDumpFile, (uint16_t)kChannels);
    WriteLe32(m_pPcmDumpFile, kSampleRate);
    WriteLe32(m_pPcmDumpFile, kSampleRate * kChannels * sizeof(int16_t));
    WriteLe16(m_pPcmDumpFile, (uint16_t)(kChannels * sizeof(int16_t)));
    WriteLe16(m_pPcmDumpFile, 16);                       // bits per sample
    fwrite("data", 1, 4, m_pPcmDumpFile);
    WriteLe32(m_pPcmDumpFile, 0);

    CLOG_INF("MIC PCM dump enabled: %s (%uHz mono S16_LE, max %ds)\n",
             dumpPath, kSampleRate, dumpSec);
}

void C_AacEnc::WritePcmDump(const short* samples, unsigned int frames)
{
    if (!m_pPcmDumpFile || !samples || frames == 0) return;
    if (m_pcmDumpMaxBytes > 0 && m_pcmDumpBytes >= m_pcmDumpMaxBytes) return;

    uint32_t bytes = frames * kChannels * sizeof(int16_t);
    if (m_pcmDumpMaxBytes > 0 && m_pcmDumpBytes + bytes > m_pcmDumpMaxBytes) {
        bytes = m_pcmDumpMaxBytes - m_pcmDumpBytes;
    }
    if (bytes == 0) return;

    const size_t n = fwrite(samples, 1, bytes, m_pPcmDumpFile);
    m_pcmDumpBytes += (uint32_t)n;
    if (m_pcmDumpBytes >= m_pcmDumpMaxBytes) {
        CLOG_INF("MIC PCM dump reached limit: %u bytes\n", m_pcmDumpBytes);
        ClosePcmDump();
    }
}

void C_AacEnc::ClosePcmDump()
{
    if (!m_pPcmDumpFile) return;

    const uint32_t dataBytes = m_pcmDumpBytes;
    fseek(m_pPcmDumpFile, 4, SEEK_SET);
    WriteLe32(m_pPcmDumpFile, 36 + dataBytes);
    fseek(m_pPcmDumpFile, 40, SEEK_SET);
    WriteLe32(m_pPcmDumpFile, dataBytes);
    fclose(m_pPcmDumpFile);
    m_pPcmDumpFile = nullptr;
    CLOG_INF("MIC PCM dump closed: %u bytes\n", dataBytes);
}

// AudioSpecificConfig (ISO/IEC 14496-3)
//   audioObjectType    : 5 bits  (AAC-LC = 2)
//   samplingFreqIndex  : 4 bits  (48000 = 3)
//   channelConfig      : 4 bits  (mono = 1)
//   总共 13 bits，凑 16 bits 即两字节
void C_AacEnc::BuildAudioSpecificConfig()
{
    static const int kFreqTable[] = {
        96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050,
        16000, 12000, 11025, 8000,  7350,  0,     0,     0
    };
    int freqIdx = 3; // 48000
    for (int i = 0; i < 13; ++i) {
        if (kFreqTable[i] == (int)kSampleRate) { freqIdx = i; break; }
    }
    int aot = 2;            // AAC-LC
    int chCfg = (int)kChannels;

    // 大端：AAAAA BBBB CCCC 000
    unsigned int v = (aot << 11) | (freqIdx << 7) | (chCfg << 3);
    m_asc[0] = (v >> 8) & 0xFF;
    m_asc[1] =  v        & 0xFF;
    m_ascLen = 2;
    CLOG_INF("AAC ASC: 0x%02x 0x%02x (aot=%d freqIdx=%d ch=%d)\n",
             m_asc[0], m_asc[1], aot, freqIdx, chCfg);
}

void C_AacEnc::CaptureEncoder()
{
    std::unique_ptr<short[]> captureBuf(new short[kAlsaPeriod * kChannels]);

    // 用于 swr_convert 输出的临时缓冲（FLTP planar）
    uint8_t** convertedData = nullptr;
    if (av_samples_alloc_array_and_samples(&convertedData, nullptr,
                                           kChannels, kAlsaPeriod,
                                           AV_SAMPLE_FMT_FLTP, 0) < 0) {
        CLOG_ERR("Could not alloc converted samples buffer\n");
        return;
    }

    while (m_bRun) {
        int err = snd_pcm_readi(m_capture_handle, captureBuf.get(), kAlsaPeriod);
        if (err == -EPIPE) {
            CLOG_ERR("ALSA underrun, recovering\n");
            snd_pcm_prepare(m_capture_handle);
            continue;
        } else if (err < 0) {
            CLOG_ERR("snd_pcm_readi error: %s\n", snd_strerror(err));
            continue;
        }

        const int framesRead = err;
        WritePcmDump(captureBuf.get(), (unsigned int)framesRead);
        ApplyMicFilter(captureBuf.get(), (unsigned int)framesRead);

        // S16 → FLTP
        const uint8_t* inData[1] = { (const uint8_t*)captureBuf.get() };
        int got = swr_convert(m_swr, convertedData, kAlsaPeriod,
                              inData, framesRead);
        if (got <= 0) continue;

        // 入 FIFO
        if (av_audio_fifo_write(m_fifo, (void**)convertedData, got) < got) {
            CLOG_ERR("av_audio_fifo_write short\n");
        }

        // 攒够 1024 就编一帧
        while (av_audio_fifo_size(m_fifo) >= (int)kAacFrameSize) {
            EncodeOneFrame();
        }
    }

    if (convertedData) {
        av_freep(&convertedData[0]);
        av_freep(&convertedData);
    }
    CLOG_INF("AAC CaptureEncoder exit\n");
}

void C_AacEnc::EncodeOneFrame()
{
    AVFrame* frame = av_frame_alloc();
    if (!frame) return;

    frame->nb_samples     = kAacFrameSize;
    frame->format         = AV_SAMPLE_FMT_FLTP;
    frame->channel_layout = AV_CH_LAYOUT_MONO;
    frame->channels       = kChannels;
    frame->sample_rate    = kSampleRate;
    if (av_frame_get_buffer(frame, 0) < 0) {
        CLOG_ERR("AAC frame get_buffer fail\n");
        av_frame_free(&frame);
        return;
    }

    if (av_audio_fifo_read(m_fifo, (void**)frame->data, kAacFrameSize)
        < (int)kAacFrameSize) {
        CLOG_ERR("av_audio_fifo_read short\n");
        av_frame_free(&frame);
        return;
    }

    frame->pts = m_nextPts;
    m_nextPts += kAacFrameSize;

    int ret = avcodec_send_frame(m_codec_ctx, frame);
    if (ret < 0) {
        PRINT_ERROR(ret);
        av_frame_free(&frame);
        return;
    }

    AVPacket* pkt = av_packet_alloc();
    while (true) {
        ret = avcodec_receive_packet(m_codec_ctx, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) { PRINT_ERROR(ret); break; }

        int64_t ptsUs = av_rescale_q(pkt->pts,
                                     m_codec_ctx->time_base,
                                     AVRational{1, 1000000});
        if (m_pDumpFile) {
            WriteAdtsAndDump(pkt->data, pkt->size);
        }
        if (m_pListener) {
            m_pListener->OnOutputAac(pkt->data, pkt->size, ptsUs);
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    av_frame_free(&frame);
}

// 给 raw AAC 加 ADTS 头并写入 dump 文件
// ADTS = 7字节固定头（本实现未使用 CRC，所以是7字节，protection_absent=1）
//   syncword         12 bits = 0xFFF
//   ID               1 bit   = 0  (MPEG-4)
//   layer            2 bits  = 0
//   protection_abs   1 bit   = 1
//   profile          2 bits  = aot - 1  (AAC-LC -> 1)
//   freq_index       4 bits
//   private          1 bit   = 0
//   channel_config   3 bits
//   ori/copy         2 bits  = 0
//   home/copyright   2 bits  = 0
//   frame_length     13 bits = 7 + raw_aac_size
//   buffer_full      11 bits = 0x7FF
//   num_raw_blk      2 bits  = 0
void C_AacEnc::WriteAdtsAndDump(const unsigned char* aac, unsigned int aacLen)
{
    if (!m_pDumpFile) return;

    static const int kFreqTable[] = {
        96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050,
        16000, 12000, 11025, 8000,  7350,  0,     0,     0
    };
    int freqIdx = 3;
    for (int i = 0; i < 13; ++i) {
        if (kFreqTable[i] == (int)kSampleRate) { freqIdx = i; break; }
    }
    const int profile = 1;            // AAC-LC = aot 2 - 1
    const int chCfg   = (int)kChannels;
    const unsigned int frameLen = 7 + aacLen;

    unsigned char adts[7];
    adts[0] = 0xFF;
    adts[1] = 0xF1;                                            // 0xF1 = MPEG-4 + protection_absent
    adts[2] = (profile << 6) | (freqIdx << 2) | (chCfg >> 2);
    adts[3] = ((chCfg & 3) << 6) | ((frameLen >> 11) & 0x03);
    adts[4] = (frameLen >> 3) & 0xFF;
    adts[5] = ((frameLen & 0x07) << 5) | 0x1F;
    adts[6] = 0xFC;

    fwrite(adts, 1, sizeof(adts), m_pDumpFile);
    fwrite(aac,  1, aacLen,       m_pDumpFile);
    fflush(m_pDumpFile);
}
