/*********************************************************************************
  *Copyright(C),Your Company
  *FileName:  logBroadcaster.h
  *Author:    gengwenguan
  *Date:      2026-07-19
  *Description:  日志广播器：把 C_LogAdapt 分叉出来的每条日志行推送给 web。
  *
  *  角色：实现 ILogSink，作为 logAdapt 与 WebSocket 之间的解耦缓冲层。
  *    业务线程（相机/编码/网络…）在 LogInner 里调用 OnLogLine —— 只做"构造
  *    字符串 + 入有界队列 + 唤醒"，绝不碰 socket；真正的广播由内部一条独立
  *    推送线程在锁外完成。这样即使 web 端弱网，也不会反压任何业务线程。
  *
  *  零空载开销：无 web 日志订阅者时（客户端计数为 0），OnLogLine 第一行即
  *    return，仅一次原子 load，不构造字符串、不入队。
  *
  *  自激防护：推送线程整个生命周期内 SetThreadLogSuppressed(true)，因此广播
  *    路径（含 WS 出错打的日志）产生的日志不会再回灌队列。
**********************************************************************************/
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include "logAdapt.h"

class C_LogBroadcaster : public ILogSink
{
public:
    // 推送回调：在推送线程里被调用（已在锁外）。terminal 用它接到
    // C_WebSocketServer::BroadcastLogText。参数为一条完整日志行（不含结尾 '\0'）。
    using PushFn = std::function<void(const char* data, size_t len)>;

    C_LogBroadcaster() = default;
    ~C_LogBroadcaster();

    // 启动推送线程。fn 必须在 Start 前确定；重复调用只第一次生效。
    void Start(PushFn fn);
    // 停止并 join 推送线程；清空残留队列。幂等。
    void Stop();

    // ILogSink：业务线程调用。无订阅者时零开销；否则入有界队列并唤醒推送线程。
    void OnLogLine(const char* line, unsigned int len) override;

    // 由 terminal 在 /ws/log 客户端握手/断开时维护。<=0 表示无人订阅。
    void SetClientCount(int n);

private:
    void PusherLoop();

    // 队列安全上限（防止推送线程卡住时无限堆积）。与 web 端 100 行显示上限无关，
    // 这里只是生产者→推送线程之间的临时缓冲，正常会被持续排空。
    static const size_t kMaxQueuedLines = 200;

    PushFn                   m_pushFn;
    std::atomic<int>         m_clientCount{0};
    std::atomic<bool>        m_running{false};
    std::thread              m_thread;

    std::mutex               m_mu;
    std::condition_variable  m_cv;
    std::deque<std::string>  m_queue;
};
