/*********************************************************************************
  *Copyright(C),Your Company
  *FileName:  talkPlayer.cpp
  *Author:    gengwenguan
  *Date:      2026-05-23
  *Description:  讲话回放：OPUS(浏览器 AudioEncoder) → ALSA(开发板扬声器)
                 ALSA 部分参数与 main.cpp#L163-272 完全一致：48kHz / S16_LE / 1ch
                 同时承担提示音 PlayWavSync(...) 的 ALSA 复用，避免对 /dev/snd
                 的多路独占开关。
**********************************************************************************/
#include "talkPlayer.h"
#include "logAdapt.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

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

bool C_TalkPlayer::EnsureAlsa_locked()
{
    if (m_pcm) return true;  // 已打开，幂等

    int err = snd_pcm_open(&m_pcm, "default", SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        CLOG_ERR("TalkPlayer snd_pcm_open: %s\n", snd_strerror(err));
        m_pcm = nullptr;
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
        snd_pcm_close(m_pcm);
        m_pcm = nullptr;
        return false;
    }
    snd_pcm_prepare(m_pcm);
    return true;
}

void C_TalkPlayer::CloseAlsa_locked()
{
    if (m_pcm) {
        snd_pcm_drop(m_pcm);
        snd_pcm_close(m_pcm);
        m_pcm = nullptr;
    }
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
    //    若 PlayWavSync 已经临时打开过 m_pcm，这里幂等复用。
    if (!EnsureAlsa_locked()) {
        Shutdown_locked();
        return false;
    }

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
#ifndef CAMERA_RUST_HOST
    {
        std::lock_guard<std::mutex> qlk(m_mtx);
        m_queue.clear();
    }
#endif

    m_run = true;
#ifdef CAMERA_RUST_HOST
    m_packet = av_packet_alloc();
    m_frame = av_frame_alloc();
    if (!m_packet || !m_frame) {
        CLOG_ERR("TalkPlayer av_packet_alloc / av_frame_alloc 失败\n");
        Shutdown_locked();
        return false;
    }
    constexpr int kMaxOutSamples = 5760;
    m_pcmBuf.resize(kMaxOutSamples * kChannels);
#else
    m_thread = std::thread(&C_TalkPlayer::DecodePlayLoop, this);
#endif
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
#ifdef CAMERA_RUST_HOST
    m_run.store(false);
#else
    if (m_run.exchange(false)) {
        m_cv.notify_all();
        if (m_thread.joinable()) m_thread.join();
    } else if (m_thread.joinable()) {
        // 极少见：m_run 已经是 false 但线程对象还 joinable（构造途中失败的兜底）
        m_thread.join();
    }
#endif

    // 2) 清积压队列（避免下次 Init 拿到上一次的尾巴）
#ifndef CAMERA_RUST_HOST
    {
        std::lock_guard<std::mutex> qlk(m_mtx);
        m_queue.clear();
    }
#endif

    // 3) 释放 FFmpeg / Swr
#ifdef CAMERA_RUST_HOST
    if (m_packet) { av_packet_free(&m_packet); }
    if (m_frame)  { av_frame_free(&m_frame); }
    std::vector<short>().swap(m_pcmBuf);
#endif
    if (m_swr)    { swr_free(&m_swr); }
    if (m_decCtx) { avcodec_free_context(&m_decCtx); }

    // 4) ALSA：若有提示音正在播，保留 m_pcm 让它播完，由 PlayWavSync 收尾时关闭。
    //    否则直接释放，让出独占的 /dev/snd。
    if (m_promptBusy.load()) {
        CLOG_INF("TalkPlayer Shutdown: prompt 正在播放，保留 m_pcm 由 prompt 收尾\n");
    } else {
        CloseAlsa_locked();
        CLOG_INF("TalkPlayer Shutdown 完成（声卡已释放）\n");
    }
}

#ifndef CAMERA_RUST_HOST
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
#endif

#ifdef CAMERA_RUST_HOST
int C_TalkPlayer::DecodePlayOpus(
    const unsigned char* data, unsigned int dataLen)
{
    if (!m_run || !m_decCtx || !m_swr || !m_packet || !m_frame) return -2;
    if (!data || dataLen == 0 || m_pcmBuf.empty()) return -1;

    av_packet_unref(m_packet);
    m_packet->data = const_cast<unsigned char*>(data);
    m_packet->size = static_cast<int>(dataLen);
    int ret = avcodec_send_packet(m_decCtx, m_packet);
    if (ret < 0) {
        PRINT_FFMPEG_ERR(ret);
        return -1;
    }

    constexpr int kMaxOutSamples = 5760;
    while (true) {
        ret = avcodec_receive_frame(m_decCtx, m_frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return 0;
        if (ret < 0) {
            PRINT_FFMPEG_ERR(ret);
            return -1;
        }

        uint8_t* out[1] = {
            reinterpret_cast<uint8_t*>(m_pcmBuf.data())
        };
        int samples = swr_convert(
            m_swr, out, kMaxOutSamples,
            (const uint8_t**)m_frame->extended_data,
            m_frame->nb_samples);
        av_frame_unref(m_frame);
        if (samples < 0) {
            PRINT_FFMPEG_ERR(samples);
            return -1;
        }
        if (samples > 0 &&
            AlsaWrite(m_pcmBuf.data(), static_cast<unsigned long>(samples)) != 0) {
            return -1;
        }
    }
}

int C_TalkPlayer::PlayPcmSync(const short* samples, unsigned long frames)
{
    if (!samples || frames == 0 || frames > 1024 * 1024) return -1;

    bool expected = false;
    if (!m_promptBusy.compare_exchange_strong(expected, true)) return -3;
    struct BusyGuard {
        std::atomic<bool>& flag;
        ~BusyGuard() { flag.store(false); }
    } busyGuard{m_promptBusy};

    bool tempOpened = false;
    {
        std::lock_guard<std::mutex> lock(m_initMtx);
        if (!m_pcm) {
            if (!EnsureAlsa_locked()) return -2;
            tempOpened = true;
        }
    }

    const int result = AlsaWrite(samples, frames);
    {
        std::lock_guard<std::mutex> lock(m_pcmMtx);
        if (m_pcm) snd_pcm_drain(m_pcm);
    }

    if (tempOpened) {
        std::lock_guard<std::mutex> lock(m_initMtx);
        if (!m_run.load()) CloseAlsa_locked();
    } else {
        std::lock_guard<std::mutex> lock(m_pcmMtx);
        if (m_pcm) snd_pcm_prepare(m_pcm);
    }
    return result == 0 ? 0 : -2;
}
#endif

void C_TalkPlayer::Reset()
{
#ifndef CAMERA_RUST_HOST
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_queue.clear();
    }
#endif
    if (m_decCtx) avcodec_flush_buffers(m_decCtx);
    {
        std::lock_guard<std::mutex> pl(m_pcmMtx);
        if (m_pcm) { snd_pcm_drop(m_pcm); snd_pcm_prepare(m_pcm); }
    }
}

int C_TalkPlayer::AlsaWrite(const short* buf, unsigned long frames)
{
    // 序列化 talk(DecodePlayLoop) 与 prompt(PlayWavSync) 对 m_pcm 的写入。
    // ALSA `default` 在 V831 上是独占设备，多个写入路径必须串行；这里通过
    // m_pcmMtx 保证两路在 period 粒度上交替提交，效果听起来类似软件 mixing。
    std::lock_guard<std::mutex> lk(m_pcmMtx);
    if (!m_pcm) return -1;

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

#ifndef CAMERA_RUST_HOST
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
#endif

#ifndef CAMERA_RUST_HOST
// ----------------------------------------------------------------------
// 提示音播放：同步阻塞 + 复用 ALSA
// ----------------------------------------------------------------------
//
// wav 头结构（标准 RIFF）：
//   off=0  "RIFF"
//   off=4  uint32 总长-8
//   off=8  "WAVE"
//   off=12 子块循环：
//          { char[4] id; uint32 size; uint8 data[size]; }
//          其中 "fmt " 描述格式、"data" 是 PCM 负载、"LIST"/"JUNK" 等需跳过。
//
// 第一版只接受：PCM(format=1) / 16-bit / 48000Hz / 1ch，与 ALSA 输出严格一致，
// 直接 memcpy 到 ALSA buffer 不做重采样；不匹配返回 -1。
//
// 需要使用其它格式的 wav 时请先用 ffmpeg 转码：
//   ffmpeg -i in.wav -ar 48000 -ac 1 -sample_fmt s16 out.wav
// ----------------------------------------------------------------------

namespace {

inline uint32_t U32LE(const uint8_t* p) {
    return  (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

inline uint16_t U16LE(const uint8_t* p) {
    return (uint16_t)(p[0] | ((uint32_t)p[1] << 8));
}

// 解析 wav 文件 → 校验格式，返回 data 子块在 buf 中的偏移和长度。
// 失败返回 false。
bool ParseWav48kMonoS16(const std::vector<uint8_t>& buf,
                       size_t& dataOff, size_t& dataLen)
{
    if (buf.size() < 44) return false;
    if (memcmp(&buf[0], "RIFF", 4) != 0) return false;
    if (memcmp(&buf[8], "WAVE", 4) != 0) return false;

    bool fmtOk = false;
    size_t off = 12;
    while (off + 8 <= buf.size()) {
        const uint8_t* p   = &buf[off];
        uint32_t       sz  = U32LE(p + 4);
        size_t         end = off + 8 + sz;
        if (end > buf.size()) return false;

        if (!memcmp(p, "fmt ", 4)) {
            if (sz < 16) return false;
            uint16_t fmtTag    = U16LE(p + 8);
            uint16_t channels  = U16LE(p + 10);
            uint32_t sampleRate= U32LE(p + 12);
            uint16_t bitsPer   = U16LE(p + 22);
            // 支持 PCM(1) 与 部分工具写出来的 EXTENSIBLE(0xFFFE) 但要求子格式仍是 PCM
            if ((fmtTag != 1 && fmtTag != 0xFFFE) ||
                channels != 1 || sampleRate != 48000 || bitsPer != 16) {
                return false;
            }
            fmtOk = true;
        } else if (!memcmp(p, "data", 4)) {
            if (!fmtOk) return false;
            dataOff = off + 8;
            dataLen = sz;
            return true;
        }
        // 跳过本子块；wav 标准要求 chunk size 奇数时补 1 字节 padding
        off = end + (sz & 1);
    }
    return false;
}

} // anonymous namespace

int C_TalkPlayer::PlayWavSync(const std::string& path)
{
    // 1) 并发守护：同时只允许一段提示音在播
    bool expected = false;
    if (!m_promptBusy.compare_exchange_strong(expected, true)) {
        CLOG_INF("TalkPlayer PlayWavSync busy, reject: %s\n", path.c_str());
        return -3;
    }
    // RAII 保证退出时复位 busy 标志
    struct BusyGuard {
        std::atomic<bool>& flag;
        ~BusyGuard() { flag.store(false); }
    } _busyGuard{ m_promptBusy };

    // 2) 读文件（door.wav 约 130KB，一次性读入足够）
    FILE* fp = fopen(path.c_str(), "rb");
    if (!fp) {
        CLOG_ERR("TalkPlayer PlayWavSync fopen 失败: %s\n", path.c_str());
        return -1;
    }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0 || sz > 8 * 1024 * 1024) {  // 单文件硬上限 8MB
        CLOG_ERR("TalkPlayer PlayWavSync 文件大小异常: %ld\n", sz);
        fclose(fp);
        return -1;
    }
    std::vector<uint8_t> buf((size_t)sz);
    size_t got = fread(buf.data(), 1, (size_t)sz, fp);
    fclose(fp);
    if (got != (size_t)sz) {
        CLOG_ERR("TalkPlayer PlayWavSync fread 不足: got=%zu sz=%ld\n", got, sz);
        return -1;
    }

    // 3) 解析 wav 头 → 校验 48k/16bit/mono
    size_t dataOff = 0, dataLen = 0;
    if (!ParseWav48kMonoS16(buf, dataOff, dataLen) || dataLen == 0) {
        CLOG_ERR("TalkPlayer PlayWavSync wav 格式不符（要求 PCM/48k/16bit/mono）: %s\n",
                 path.c_str());
        return -1;
    }

    // 4) 确保 ALSA 已打开；若未打开则临时打开，播完后看是否要释放
    bool tempOpened = false;
    {
        std::lock_guard<std::mutex> lk(m_initMtx);
        if (!m_pcm) {
            if (!EnsureAlsa_locked()) {
                CLOG_ERR("TalkPlayer PlayWavSync EnsureAlsa 失败\n");
                return -2;
            }
            tempOpened = true;
        }
    }

    // 5) 灌入 ALSA：参数完全匹配，直接当 S16 mono 帧写
    const short*   pcm    = reinterpret_cast<const short*>(&buf[dataOff]);
    unsigned long  frames = dataLen / sizeof(short);  // mono: 1 sample = 1 frame
    int wr = AlsaWrite(pcm, frames);

    // 6) 等待 buffer 排空（drain），再决定是否归还 ALSA。
    //    drain 必须在持锁时调用，否则会和 talk 的 writei 并发。
    {
        std::lock_guard<std::mutex> pl(m_pcmMtx);
        if (m_pcm) snd_pcm_drain(m_pcm);
    }

    // 7) 临时打开的 ALSA：若整段播放期间没有 talk 客户端把它接管走，则归还。
    //    （正常路径下 m_run==false 表示没有 talk 在线）
    if (tempOpened) {
        std::lock_guard<std::mutex> lk(m_initMtx);
        if (!m_run.load()) {
            CloseAlsa_locked();
            CLOG_INF("TalkPlayer PlayWavSync 临时声卡已归还\n");
        }
    } else {
        // 复用 talk 的 ALSA：drain 后用 prepare 把 PCM 状态拉回 SETUP，
        // 让 talk 的下一次 writei 不会因为 drain 进入 DRAIN 状态而失败。
        std::lock_guard<std::mutex> pl(m_pcmMtx);
        if (m_pcm) snd_pcm_prepare(m_pcm);
    }

    if (wr != 0) {
        CLOG_ERR("TalkPlayer PlayWavSync AlsaWrite 失败\n");
        return -2;
    }
    return 0;
}
#endif
