/*********************************************************************************
  *Copyright(C),Your Company
  *FileName:  liveHub.cpp
  *Author:    gengwenguan
  *Date:      2026-05-23
  *Description:  LiveHub 实现
**********************************************************************************/
#include "liveHub.h"
#include "logAdapt.h"

void C_LiveHub::PushInitSegment(const uint8_t* data, size_t len)
{
    std::lock_guard<std::mutex> lk(m_mu);
    m_initSeg.assign(data, data + len);
    CLOG_INF("LiveHub init segment cached: %zu bytes\n", len);
    // 当前已订阅的 listener 也补发一次（防止它在 muxer 出 init 之前已经订阅）
    for (auto* l : m_listeners) {
        l->OnLiveInitSegment(m_initSeg.data(), m_initSeg.size());
    }
}

void C_LiveHub::PushFragment(const uint8_t* data, size_t len)
{
    std::lock_guard<std::mutex> lk(m_mu);
    for (auto* l : m_listeners) {
        l->OnLiveFragment(data, len);
    }
}

void C_LiveHub::Subscribe(C_Listener* l)
{
    if (!l) return;
    std::lock_guard<std::mutex> lk(m_mu);
    m_listeners.insert(l);
    if (!m_initSeg.empty()) {
        // 立刻把 init segment 推给新订阅者
        l->OnLiveInitSegment(m_initSeg.data(), m_initSeg.size());
    }
    CLOG_INF("LiveHub Subscribe: now %zu listeners\n", m_listeners.size());
}

void C_LiveHub::Unsubscribe(C_Listener* l)
{
    if (!l) return;
    std::lock_guard<std::mutex> lk(m_mu);
    m_listeners.erase(l);
    CLOG_INF("LiveHub Unsubscribe: now %zu listeners\n", m_listeners.size());
}
