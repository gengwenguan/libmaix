/*********************************************************************************
  *Copyright(C),Your Company
  *FileName:  talkPlayer.cpp
  *Author:    gengwenguan
  *Date:      2026-05-23
  *Description:  讲话回放：OPUS(浏览器 AudioEncoder) → ALSA(开发板扬声器)
                 ALSA 部分参数与 main.cpp#L163-272 完全一致：48kHz / S16_LE / 1ch
**********************************************************************************/
#include "talkPlayer.h"
#include "logAdapt.h"
#include <chrono>

#define PRINT_FFMPEG_ERR(errnum) do { \
    char errbuf[AV_ERROR_MAX_STRING_SIZE]; \
    av_strerror(errnum, errbuf, sizeof(errbuf)); \
    CLOG_ERR("TalkPlayer ffmpeg err: %s (%d)\n", errbuf, errnum); \
} while (0)

C_TalkPlayer::C_TalkPlayer()
{
    // 构造函数刻意留空：不打开 ALSA / FFmpeg / 解码线程。
    // 重资源（独占声卡、解码器内存、解码线程）全部延迟到第一个 /ws/talk
    // 客户端连入时由 InitDevice() 打开；最后一个客户端断开时由 Shutdown()
    // 释放，让出 /dev/snd 给 TTS / 提示音 / 报警声等其它播放路径。
    CLOG_INF("TalkPlayer 已创建（懒加载，等待 /ws/talk 连入）\n");
}

C_TalkPlayer::~C_TalkPlayer()
{
    Shutdown();
}

bool C_TalkPlayer::InitDevice()
{
    std::lock_guard<std::mutex> lk(m_initMtx);

    // 已经 Init 过：幂等返回成功（多个 /ws/talk 客户端同时连入也只 Init 一次）
    if (m_run.load()) {
        CLOG_INF("TalkPlayer InitDevice: already running, skip\n");
        return true;
    }

    // 1) ALSA 播放设备（与 main.cpp 测试代码相同参数）
    int err = snd_pcm_open(&m_pcm, "default", SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        CLOG_ERR("TalkPlayer snd_pcm_open: %s\n", snd_strerror(err));
        m_pcm = nullptr;
        Shutdown_locked();
        return false;
    }

    snd_pcm_hw_params_t* params = nullptr;
    snd_pcm_hw_params_malloc(&params);
    snd_pcm_hw_params_any(m_pcm, params);
    snd_pcm_hw_params_set_access(m_pcm, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(m_pcm, params, SND_PCM_FORMAT_S16_LE);
    snd_pcm_hw_params_set_channels(m_pcm, params, kChannels);
    unsigned int rate = kSampleRate;
    int dir = 0;
    snd_pcm_hw_params_set_rate_near(m_pcm, params, &rate, &dir);
    unsigned int buffer_time = 60 * 1000;  // 60ms
    snd_pcm_hw_params_set_buffer_time_near(m_pcm, params, &buffer_time, &dir);
    unsigned long period = kAlsaPeriod;
    snd_pcm_hw_params_set_period_size_near(m_pcm, params, &period, &dir);
    err = snd_pcm_hw_params(m_pcm, params);
    snd_pcm_hw_params_free(params);
    if (err < 0) {
        CLOG_ERR("TalkPlayer snd_pcm_hw_params: %s\n", snd_strerror(err));
        Shutdown_locked();
        return false;
    }
    snd_pcm_prepare(m_pcm);

    // 2) FFmpeg OPUS 解码器
    AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_OPUS);
    if (!codec) {
        CLOG_ERR("TalkPlayer 找不到 AV_CODEC_ID_OPUS 解码器\n");
        Shutdown_locked();
        return false;
    }
    m_decCtx = avcodec_alloc_context3(codec);
    if (!m_decCtx) {
        CLOG_ERR("TalkPlayer avcodec_alloc_context3 失败\n");
        Shutdown_locked();
        return false;
    }
    m_decCtx->sample_rate    = kSampleRate;
    m_decCtx->channels       = kChannels;
    m_decCtx->channel_layout = AV_CH_LAYOUT_MONO;
    // OPUS 解码器内部固定输出 FLTP，外部根据 frame->format 动态处理

    if (avcodec_open2(m_decCtx, codec, nullptr) < 0) {
        CLOG_ERR("TalkPlayer avcodec_open2 失败\n");
        Shutdown_locked();
        return false;
    }

    // 3) swr：FLTP → S16 interleaved（OPUS 解码出来基本都是 FLTP）
    m_swr = swr_alloc();
    av_opt_set_int       (m_swr, "in_channel_layout",  AV_CH_LAYOUT_MONO,  0);
    av_opt_set_int       (m_swr, "out_channel_layout", AV_CH_LAYOUT_MONO,  0);
    av_opt_set_int       (m_swr, "in_sample_rate",     kSampleRate,        0);
    av_opt_set_int       (m_swr, "out_sample_rate",    kSampleRate,        0);
    av_opt_set_sample_fmt(m_swr, "in_sample_fmt",      AV_SAMPLE_FMT_FLTP, 0);
    av_opt_set_sample_fmt(m_swr, "out_sample_fmt",     AV_SAMPLE_FMT_S16,  0);
    if (swr_init(m_swr) < 0) {
        CLOG_ERR("TalkPlayer swr_init 失败\n");
        Shutdown_locked();
        return false;
    }

    // 清空可能残留的旧队列（理论上 Shutdown 已经清空过，这里防御一次）
    {
        std::lock_guard<std::mutex> qlk(m_mtx);
        m_queue.clear();
    }

    m_run = true;
    m_thread = std::thread(&C_TalkPlayer::DecodePlayLoop, this);
    CLOG_INF("TalkPlayer InitDevice 完成: 48kHz mono S16_LE\n");
    return true;
}

void C_TalkPlayer::Shutdown()
{
    std::lock_guard<std::mutex> lk(m_initMtx);
    Shutdown_locked();
}

void C_TalkPlayer::Shutdown_locked()
{
    // 1) 停解码线程
    if (m_run.exchange(false)) {
        m_cv.notify_all();
        if (m_thread.joinable()) m_thread.join();
    } else if (m_thread.joinable()) {
        // 极少见：m_run 已经是 false 但线程对象还 joinable（构造途中失败的兜底）
        m_thread.join();
    }

    // 2) 清积压队列（避免下次 Init 拿到上一次的尾巴）
    {
        std::lock_guard<std::mutex> qlk(m_mtx);
        m_queue.clear();
    }

    // 3) 释放 FFmpeg / Swr / ALSA
    if (m_swr)    { swr_free(&m_swr); }
    if (m_decCtx) { avcodec_free_context(&m_decCtx); }
    if (m_pcm)    {
        // 注意：drain 会阻塞直到 ALSA buffer 放完；这里直接 drop 抛弃残留 PCM，
        // 因为客户端已经断开，没必要再播放上一次的尾音
        snd_pcm_drop(m_pcm);
        snd_pcm_close(m_pcm);
        m_pcm = nullptr;
    }
    CLOG_INF("TalkPlayer Shutdown 完成（声卡已释放）\n");
}

int C_TalkPlayer::FeedOpus(const unsigned char* data, unsigned int dataLen)
{
    if (!m_run || !m_decCtx) return -2;
    if (!data || dataLen == 0) return -1;

    {
        std::lock_guard<std::mutex> lk(m_mtx);
        if (m_queue.size() >= kQueueMax) {
            // 太多积压：丢最旧（避免延迟越来越大）
            m_queue.pop_front();
        }
        m_queue.emplace_back(data, data + dataLen);
    }
    m_cv.notify_one();
    return 0;
}

void C_TalkPlayer::Reset()
{
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_queue.clear();
    }
    if (m_decCtx) avcodec_flush_buffers(m_decCtx);
    if (m_pcm)    { snd_pcm_drop(m_pcm); snd_pcm_prepare(m_pcm); }
}

int C_TalkPlayer::AlsaWrite(const short* buf, unsigned long frames)
{
    unsigned long off = 0;
    while (off < frames) {
        snd_pcm_sframes_t w = snd_pcm_writei(m_pcm, buf + off * kChannels,
                                             frames - off);
        if (w == -EPIPE) {
            CLOG_ERR("TalkPlayer ALSA underrun, recover\n");
            snd_pcm_prepare(m_pcm);
            continue;
        }
        if (w == -EAGAIN) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        if (w < 0) {
            CLOG_ERR("TalkPlayer snd_pcm_writei: %s\n", snd_strerror((int)w));
            snd_pcm_prepare(m_pcm);
            return -1;
        }
        off += (unsigned long)w;
    }
    return 0;
}

void C_TalkPlayer::DecodePlayLoop()
{
    AVPacket* pkt   = av_packet_alloc();
    AVFrame*  frame = av_frame_alloc();
    if (!pkt || !frame) {
        CLOG_ERR("TalkPlayer av_packet_alloc / av_frame_alloc 失败\n");
        if (pkt)   av_packet_free(&pkt);
        if (frame) av_frame_free(&frame);
        return;
    }

    // 输出 PCM 缓冲：单帧最多 120ms（OPUS 单帧最大），48k * 0.12 = 5760 samples
    constexpr int kMaxOutSamples = 5760;
    std::vector<short> pcmBuf(kMaxOutSamples * kChannels);

    while (m_run) {
        std::vector<unsigned char> opus;
        {
            std::unique_lock<std::mutex> lk(m_mtx);
            m_cv.wait_for(lk, std::chrono::milliseconds(100), [this]{
                return !m_run || !m_queue.empty();
            });
            if (!m_run) break;
            if (m_queue.empty()) continue;
            opus = std::move(m_queue.front());
            m_queue.pop_front();
        }

        // 喂给解码器
        pkt->data = opus.data();
        pkt->size = (int)opus.size();
        int ret = avcodec_send_packet(m_decCtx, pkt);
        if (ret < 0) {
            PRINT_FFMPEG_ERR(ret);
            continue;
        }

        while (true) {
            ret = avcodec_receive_frame(m_decCtx, frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) { PRINT_FFMPEG_ERR(ret); break; }

            // FLTP → S16 interleaved
            uint8_t* out[1] = { reinterpret_cast<uint8_t*>(pcmBuf.data()) };
            int n = swr_convert(m_swr, out, kMaxOutSamples,
                                (const uint8_t**)frame->extended_data,
                                frame->nb_samples);
            if (n > 0) {
                AlsaWrite(pcmBuf.data(), (unsigned long)n);
            }
            av_frame_unref(frame);
        }
    }

    av_packet_free(&pkt);
    av_frame_free(&frame);
}
