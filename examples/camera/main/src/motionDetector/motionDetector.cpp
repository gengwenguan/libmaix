/*********************************************************************************
  *Copyright(C),Your Company
  *FileName:  motionDetector.cpp
  *Author:    gengwenguan
  *Date:      2026-05-24
  *Description:  视频移动侦测实现。详见 motionDetector.h 顶部注释。
**********************************************************************************/
#include "motionDetector.h"
#include "appConfig.h"
#include "snapshot.h"
#include "logAdapt.h"

#include <chrono>
#include <cstdlib>
#include <cstring>

namespace {
inline int64_t NowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
} // namespace

C_MotionDetector::C_MotionDetector(int width, int height)
    : m_width(width), m_height(height)
{
    // 4800 字节，懒分配也行；这里直接 reserve，省得后面 push_back 触发 realloc
    m_prev.assign((size_t)kDownW * kDownH, 0);
}

C_MotionDetector::~C_MotionDetector()
{
    Stop();
}

int C_MotionDetector::Start()
{
    if (m_running.load()) return 0;
    m_running.store(true);
    m_pendingTrigger.store(false);
    m_haveRef       = false;
    m_lastCheckMs   = 0;
    m_lastTriggerMs = 0;
    m_dispatchTh = std::thread(&C_MotionDetector::DispatchLoop, this);
    CLOG_INF("motionDetector: started (w=%d h=%d down=%dx%d)\n",
             m_width, m_height, kDownW, kDownH);
    return 0;
}

void C_MotionDetector::Stop()
{
    if (!m_running.exchange(false)) return;
    {
        std::lock_guard<std::mutex> lk(m_cvMu);
        m_pendingTrigger.store(false);
    }
    m_cv.notify_all();
    if (m_dispatchTh.joinable()) m_dispatchTh.join();
    CLOG_INF("motionDetector: stopped\n");
}

// --------------------------------------------------------------------
// 由相机回调线程每帧调用 —— 必须 < 1ms，否则会冲掉 30Hz 主循环
//   关闭分支：一次 atomic load + 一次 GetSnapshot（lock-free 快照），立即返回
//   开启分支：节流后做 80×60 = 4800 像素的差分（< 50us）
// --------------------------------------------------------------------
void C_MotionDetector::InputNv21(const unsigned char* nv21)
{
    if (!nv21) return;
    if (!m_running.load(std::memory_order_relaxed)) return;

    auto cfg = C_AppConfig::GetInst().GetSnapshot();
    if (!cfg.vmd_enabled) {
        // 开关关上时清状态：下次重新打开就从首帧起算，避免误触发
        if (m_haveRef) {
            m_haveRef = false;
            m_lastRatio.store(0.0f);
        }
        return;
    }

    // 1. 节流：vmd_check_fps（1~30），周期 = 1000/fps ms
    int fps = cfg.vmd_check_fps;
    if (fps < 1)  fps = 1;
    if (fps > 30) fps = 30;
    int64_t periodMs = 1000 / fps;
    int64_t now = NowMs();
    if (m_lastCheckMs != 0 && (now - m_lastCheckMs) < periodMs) {
        return;
    }
    m_lastCheckMs = now;

    // 2. 下采样 NV21 的 Y 平面：每 8x8 取 1 个像素 → 80×60
    //    为什么不算均值：4800 个像素的差分已经够稳，做均值反而把"小目标"模糊掉
    //    （比如远处行人占画面 1%，下采均值会把它平掉）
    const int strideX = m_width  / kDownW;   // 8
    const int strideY = m_height / kDownH;   // 8
    if (strideX < 1 || strideY < 1) return;  // 防御：相机分辨率太小

    uint8_t cur[kDownW * kDownH];
    {
        const unsigned char* y = nv21;  // Y 平面就是前 W*H 字节
        int dst = 0;
        for (int dy = 0; dy < kDownH; ++dy) {
            const unsigned char* row = y + (dy * strideY) * m_width;
            for (int dx = 0; dx < kDownW; ++dx) {
                cur[dst++] = row[dx * strideX];
            }
        }
    }

    // 3. 首帧只填 ref，不比较
    if (!m_haveRef) {
        std::memcpy(m_prev.data(), cur, sizeof(cur));
        m_haveRef = true;
        return;
    }

    // 4. 差分：abs(cur - prev) > vmd_pixel_thresh 的像素计数
    int thresh = cfg.vmd_pixel_thresh;
    if (thresh < 1)   thresh = 1;
    if (thresh > 255) thresh = 255;
    int diffCount = 0;
    for (int i = 0; i < kDownW * kDownH; ++i) {
        int d = (int)cur[i] - (int)m_prev[i];
        if (d < 0) d = -d;
        if (d > thresh) ++diffCount;
    }

    // 5. 把当前帧覆盖为 ref —— "相邻帧差"语义，对光线缓变天然不敏感
    std::memcpy(m_prev.data(), cur, sizeof(cur));

    float ratio = (float)diffCount / (float)(kDownW * kDownH);
    m_lastRatio.store(ratio);

    // 6. 命中 → 异步派发
    if (ratio > cfg.vmd_area_ratio) {
        // 注意：throttle（vmd_min_interval_s）放到 DispatchLoop 里做，
        // 这里只是"通知有事发生"。这样即使 60s 内连续运动，主循环里也只会被
        // notify 60/2 = 30 次，每次都立即 reset pending，开销可忽略。
        m_pendingTrigger.store(true);
        m_cv.notify_one();
    }
}

// --------------------------------------------------------------------
// 后台线程：等触发信号 → 节流判断 → 调 Snapshot::TakeOne()
// --------------------------------------------------------------------
void C_MotionDetector::DispatchLoop()
{
    CLOG_INF("motionDetector: dispatch loop start\n");
    while (m_running.load()) {
        // 等：要么被触发，要么被 Stop() 唤醒
        std::unique_lock<std::mutex> lk(m_cvMu);
        m_cv.wait(lk, [this] {
            return !m_running.load() || m_pendingTrigger.load();
        });
        if (!m_running.load()) break;
        m_pendingTrigger.store(false);
        lk.unlock();

        // 节流：vmd_min_interval_s 秒内只触发一次
        auto cfg = C_AppConfig::GetInst().GetSnapshot();
        int64_t now = NowMs();
        int64_t minIntervalMs = (int64_t)cfg.vmd_min_interval_s * 1000;
        if (m_lastTriggerMs != 0 && (now - m_lastTriggerMs) < minIntervalMs) {
            // 冷却中，丢弃本次
            continue;
        }
        m_lastTriggerMs = now;

        if (m_pSnapshot) {
            std::string name = m_pSnapshot->TakeOne();
            if (!name.empty()) {
                CLOG_INF("motionDetector: motion detected (ratio=%.3f), snapshot -> %s\n",
                         m_lastRatio.load(), name.c_str());
            } else {
                CLOG_WRN("motionDetector: motion detected but TakeOne returned empty\n");
            }
        }
    }
    CLOG_INF("motionDetector: dispatch loop exit\n");
}
