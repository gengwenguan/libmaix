/*********************************************************************************
  *Copyright(C),Your Company
  *FileName:  talkPlayer.h
  *Author:    gengwenguan
  *Date:      2026-05-23
  *Description:  浏览器→开发板 单向语音对讲（"讲话"按钮）的播放端：
                 接收浏览器 WebCodecs/AudioEncoder 编出的 OPUS 帧，
                 用 FFmpeg libavcodec 解码 → S16LE 48kHz 单声道 → ALSA 扬声器播放。
                 复用 main.cpp#L163-272 的 ALSA 输出流程（48000Hz / S16_LE / 1ch / period=960）。

                 资源生命周期（懒加载）：
                   - 构造时**不**打开 ALSA / FFmpeg 解码器，这些重资源全部延迟到第
                     一个 /ws/talk 客户端连入时再 InitDevice() 开。
                   - 最后一个 /ws/talk 客户端断开时 Shutdown() 释放，让出独占的
                     /dev/snd 给 TTS / 提示音等其它用法。
                   - InitDevice/Shutdown 之间互斥；FeedOpus 在未 Init 时直接丢弃。
                   - 由 C_Terminal 通过引用计数（m_talkFds 大小 0↔1 切换）触发。

                 提示音 PlayWavSync(...)：
                   - 与对讲共享同一个 m_pcm（ALSA `default` 在 V831 上独占）。
                   - 通过 m_pcmMtx 序列化对 m_pcm 的写入，避免与 DecodePlayLoop
                     并发写造成的混音/底层错误。
                   - m_promptBusy CAS 守护并发：同一时刻只允许一段提示音在播。
                   - 播放期间若没有 talk 客户端在线，临时打开 m_pcm 仅供本次使用，
                     播完后若仍无 talk 客户端则收尾关闭，归还 ALSA。
**********************************************************************************/
#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" {
    #include <alsa/asoundlib.h>
    #include <libavutil/avutil.h>
    #include <libavutil/opt.h>
    #include <libavcodec/avcodec.h>
    #include <libswresample/swresample.h>
}

class C_TalkPlayer
{
private:
    static constexpr unsigned int  kSampleRate = 48000;
    static constexpr unsigned int  kChannels   = 1;
    static constexpr unsigned long kAlsaPeriod = 960;     // 20ms @ 48k
    static constexpr unsigned int  kQueueMax   = 64;      // 最多缓冲 64 帧 ≈ 1.3s

public:
    C_TalkPlayer();
    ~C_TalkPlayer();

    // 打开 ALSA + FFmpeg + 启动解码线程。已经 Init 时直接返回 true。
    // 由 C_Terminal 在第一个 /ws/talk 客户端握手成功时调用。
    // 失败时已 Shutdown 内部资源，下一次连入会再尝试一次。
    bool InitDevice();

    // 停止解码线程 + 释放 ALSA / FFmpeg / SwrContext，把声卡让出来。
    // 由 C_Terminal 在最后一个 /ws/talk 客户端断开时调用。
    // 注意：若当前正有 PlayWavSync 在执行，会保留 m_pcm 让提示音播完，
    //       由提示音收尾时再关闭 ALSA。
    void Shutdown();

    // 是否处于已 Init 状态（用于上层判断要不要 FeedOpus）
    bool IsRunning() const { return m_run.load(); }

    // 收到一帧 OPUS（一帧 = 浏览器 AudioEncoder 一个 EncodedAudioChunk）
    // 不阻塞调用方：拷贝入队后立即返回；解码/播放在内部线程里完成。
    // 返回值：0 入队成功；-1 队列满（丢弃）；-2 未运行（被静默丢弃，调用方不必报错）
    int FeedOpus(const unsigned char* data, unsigned int dataLen);

    // 客户端断开时调用：清空队列 + 重置解码器/PCM 状态，避免拼到下一次会话
    void Reset();

    // 同步播放一段 16-bit / 48kHz / mono PCM WAV 文件（与 ALSA 输出参数一致，零转码）。
    // 与对讲共用同一 ALSA 设备：talk 在线时 prompt 写入会和 talk 解码线程通过
    // m_pcmMtx 序列化；talk 不在线时 prompt 临时打开 ALSA、播完归还。
    // 整个调用阻塞约等于 wav 时长（door.wav ≈ 1.4s）。
    // 返回值：
    //    0 播放完成
    //   -1 文件读取/解析失败（路径不存在、wav 头不对、格式不匹配）
    //   -2 ALSA 不可用（独占被其它进程拿走 / hw_params 失败）
    //   -3 已有提示音在播（busy）—— HTTP 层应回 409
    int PlayWavSync(const std::string& path);

private:
    void DecodePlayLoop();

    // ALSA 写入封装（处理 underrun）。AlsaWrite 自带 m_pcmMtx 锁定，
    // 调用方无需持锁。
    int  AlsaWrite(const short* buf, unsigned long frames);

    // 内部 Shutdown：调用方需自行持有 m_initMtx
    void Shutdown_locked();

    // 幂等地确保 m_pcm 已打开。需调用方持 m_initMtx。
    bool EnsureAlsa_locked();

    // 关闭 m_pcm（drop + close + 置 nullptr）。需调用方持 m_initMtx。
    void CloseAlsa_locked();

private:
    snd_pcm_t*        m_pcm        = nullptr;
    AVCodecContext*   m_decCtx     = nullptr;
    SwrContext*       m_swr        = nullptr;     // 解码出来若是 FLTP，则转 S16

    std::atomic<bool> m_run{false};
    std::thread       m_thread;

    std::mutex                            m_mtx;     // 保护 m_queue
    std::condition_variable               m_cv;
    std::deque<std::vector<unsigned char>> m_queue;

    std::mutex        m_initMtx;   // 保护 InitDevice / Shutdown 串行化
    std::mutex        m_pcmMtx;    // 序列化对 m_pcm 的并发写入（talk vs prompt）

    // CAS 守护：同时只允许一段提示音在播（避免重叠写入产生啸叫）。
    std::atomic<bool> m_promptBusy{false};
};
