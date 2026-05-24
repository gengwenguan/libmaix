/*********************************************************************************
  *Copyright(C),Your Company
  *FileName:  motionDetector.h
  *Author:    gengwenguan
  *Date:      2026-05-24
  *Description:  视频移动侦测（VMD：Video Motion Detection）。
  *
  *  与 PersonDetector 平行：PersonDetector 用 NPU + AI 模型识别"人形"，
  *  本模块用纯 CPU 计算"画面是否在变化"，不识别物体类型。
  *  优点：开销极低（小于一次 480p 全帧 memcpy），不依赖模型，0 误检（夜间空房 = 0 触发）。
  *  典型用法：与 PersonDetector 同时启用，事件量大幅降低（白天 AI / 夜间 VMD），
  *           或单独启用（无 NPU 模型场景）。
  *
  *  算法（极简够用）：
  *    1. 入口 InputNv21() 由相机线程每帧调用：
  *         - 用 AppConfig.vmd_check_fps 节流：达不到周期就直接 return
  *         - 取 NV21 的 Y 平面（前 W*H 字节）下采样到 80×60（按 8x8 取一个像素）
  *         - 与上一参考帧 luma 做差，绝对值 > vmd_pixel_thresh 的像素数 / (80*60)
  *           > vmd_area_ratio 即判定"有移动"
  *         - 命中后按 vmd_min_interval_s 节流，调 Snapshot::TakeOne()
  *    2. 参考帧每次都更新（"相邻两帧差"语义），简单且对光线缓变天然不敏感。
  *
  *  线程模型：
  *    - InputNv21() 由相机回调线程同步调用（30Hz），整个差分计算 + 触发判定都在
  *      调用线程内完成；80×60 = 4800 个 byte，差分本身 < 50us，对主循环无影响。
  *    - 触发拍照时调 m_pSnapshot->TakeOne()，TakeOne 内部会等下一帧 + 编码 + 写盘
  *      （~50ms），如果阻塞了主循环会丢 1 帧。所以这里**异步**派发：把"已触发"
  *      标志设上，由内部独立线程把 TakeOne 跑掉。
  *
  *  线程安全：
  *    - InputNv21 单调用方（相机回调线程），m_prev 仅它写读，无锁
  *    - 触发派发用 condition_variable + 简单计数标志
**********************************************************************************/
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class C_Snapshot;

class C_MotionDetector
{
public:
    // width/height：相机原始 NV21 尺寸（取 Y 平面前 W*H 字节）
    C_MotionDetector(int width, int height);
    ~C_MotionDetector();

    // 绑定 Snapshot 模块（命中后触发拍照）
    void SetSnapshot(C_Snapshot* snap) { m_pSnapshot = snap; }

    // 启动后台触发派发线程。失败返回非 0；当前实现只有线程启动失败这一种
    // 错误，其它字段在 InputNv21 调用时按需懒分配。
    int Start();

    // 停止派发线程并清理状态。可重入。
    void Stop();

    // 由相机回调线程每帧调用。enabled=false 时仅原子 load 立即返回，开销几乎为零。
    void InputNv21(const unsigned char* nv21);

    // 调试用：最近一次差分得到的"变化像素占比"（0~1），可暴露给前端做参数调优指引
    float LastChangeRatio() const { return m_lastRatio.load(); }

private:
    void DispatchLoop();
    static constexpr int kDownW = 80;
    static constexpr int kDownH = 60;

    int           m_width  = 0;
    int           m_height = 0;
    C_Snapshot*   m_pSnapshot = nullptr;

    // 上一参考帧（80×60 灰度，4800 字节）；首帧 m_haveRef=false 时只填充不比较
    std::vector<uint8_t> m_prev;
    bool                 m_haveRef = false;

    // 节流：上次做差分判定的时间戳（毫秒）
    int64_t              m_lastCheckMs = 0;
    // 节流：上次触发拍照的时间戳（毫秒）
    int64_t              m_lastTriggerMs = 0;
    std::atomic<float>   m_lastRatio{0.0f};

    // ---- 异步派发 TakeOne ----
    // 不在相机线程同步调 TakeOne，避免它内部 wait_for(1s) 拖累 30Hz 主循环。
    std::thread             m_dispatchTh;
    std::atomic<bool>       m_running{false};
    std::atomic<bool>       m_pendingTrigger{false};
    std::mutex              m_cvMu;
    std::condition_variable m_cv;
};
