/*********************************************************************************
  *Copyright(C),Your Company
  *FileName:  liveHub.h
  *Author:    gengwenguan
  *Date:      2026-05-23
  *Description:  直播总线：缓存 init segment + 多 listener 广播 fragment
                 - 来自 fmp4Muxer 的 init segment 缓存于此（首个订阅者拿历史）
                 - 来自 fmp4Muxer 的 fragment 实时广播给所有订阅者（不做历史缓存）
                 - 订阅顺序：先发 init segment，再开始接 fragment
**********************************************************************************/
#pragma once
#include <vector>
#include <mutex>
#include <unordered_set>
#include <cstdint>

class C_LiveHub
{
public:
    class C_Listener {
    public:
        virtual ~C_Listener() = default;
        // 订阅时如果 hub 已有 init segment，会立即调用一次
        virtual void OnLiveInitSegment(const uint8_t* data, size_t len) = 0;
        // 实时 fragment 广播（每个 keyframe 一个，约 1~2s 一次）
        virtual void OnLiveFragment(const uint8_t* data, size_t len) = 0;
    };

public:
    static C_LiveHub& Inst() {
        static C_LiveHub s;
        return s;
    }

    // muxer 侧入口
    void PushInitSegment(const uint8_t* data, size_t len);
    void PushFragment   (const uint8_t* data, size_t len);

    // 订阅 / 取消订阅（订阅时若已有 init segment 会立即推送）
    void Subscribe  (C_Listener* l);
    void Unsubscribe(C_Listener* l);

private:
    C_LiveHub() = default;
    C_LiveHub(const C_LiveHub&) = delete;
    C_LiveHub& operator=(const C_LiveHub&) = delete;

    std::mutex                       m_mu;
    std::vector<uint8_t>             m_initSeg;
    std::unordered_set<C_Listener*>  m_listeners;
};
