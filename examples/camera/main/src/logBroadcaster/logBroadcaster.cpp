/*********************************************************************************
  *Copyright(C),Your Company
  *FileName:  logBroadcaster.cpp
  *Author:    gengwenguan
  *Date:      2026-07-19
  *Description:  日志广播器实现。详见 logBroadcaster.h。
**********************************************************************************/
#include "logBroadcaster.h"

C_LogBroadcaster::~C_LogBroadcaster()
{
    Stop();
}

void C_LogBroadcaster::Start(PushFn fn)
{
    bool expected = false;
    if (!m_running.compare_exchange_strong(expected, true)) return;
    m_pushFn = std::move(fn);
    m_thread = std::thread(&C_LogBroadcaster::PusherLoop, this);
}

void C_LogBroadcaster::Stop()
{
    if (!m_running.exchange(false)) return;
    m_cv.notify_all();
    if (m_thread.joinable()) m_thread.join();
    std::lock_guard<std::mutex> lk(m_mu);
    m_queue.clear();
}

void C_LogBroadcaster::SetClientCount(int n)
{
    m_clientCount.store(n < 0 ? 0 : n, std::memory_order_release);
    // 从 0→N 或数量变化时唤醒推送线程（虽然入队时也会 notify，这里保证及时性）。
    m_cv.notify_one();
}

void C_LogBroadcaster::OnLogLine(const char* line, unsigned int len)
{
    // 无订阅者：一次原子 load 即返回，不构造 std::string、不入队、不加锁。
    if (m_clientCount.load(std::memory_order_acquire) <= 0) return;
    if (!line || len == 0) return;

    // 在锁外构造字符串（堆分配移出临界区，减少锁持有时间）。
    std::string s(line, len);
    {
        std::lock_guard<std::mutex> lk(m_mu);
        // 队列过长（推送线程被弱网卡住）时丢弃最早的行，保证有界。
        while (m_queue.size() >= kMaxQueuedLines) m_queue.pop_front();
        m_queue.emplace_back(std::move(s));
    }
    m_cv.notify_one();
}

void C_LogBroadcaster::PusherLoop()
{
    // 防御性：把本推送线程标记为"不再向 sink 分叉"。当前广播链路
    //（m_pushFn → BroadcastLogText → QueueBinary）并不打日志，所以此刻是 no-op；
    // 但只要将来有人在这条链路里加一句 CLOG_*，那条日志就会经 sink 再次入队，被本
    // 线程取出后又广播、又打印，形成永不排空的自激循环。这里一次性设好，杜绝隐患。
    // thread_local，仅影响本推送线程，不干扰任何业务线程。
    SetThreadLogSuppressed(true);

    while (m_running.load(std::memory_order_acquire)) {
        std::deque<std::string> batch;
        {
            std::unique_lock<std::mutex> lk(m_mu);
            m_cv.wait(lk, [this] {
                return !m_running.load(std::memory_order_acquire) || !m_queue.empty();
            });
            if (!m_running.load(std::memory_order_acquire) && m_queue.empty()) break;
            // 一次性 swap 出全部待发行，随后锁外发送，缩短临界区。
            batch.swap(m_queue);
        }
        if (!m_pushFn) continue;
        for (const auto& line : batch) {
            // 再判一次订阅者：推送期间可能全部断开，避免无谓 IO。
            if (m_clientCount.load(std::memory_order_acquire) <= 0) break;
            m_pushFn(line.data(), line.size());
        }
    }
}
