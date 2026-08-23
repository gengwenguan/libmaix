/*********************************************************************************
  *Copyright(C),Your Company
  *FileName:  personDetector.h
  *Author:    gengwenguan
  *Date:      2026-05-23
  *Description:  人形识别 → 自动拍照模块（V831 awnn / NPU）。
  *
  *  设计要点：
  *    1. 模型与配置：参考 examples/nn_yolo_person，YOLOv2 单类（person）
  *         - 模型路径：/root/models/awnn_yolo_person.{bin,param}（用户自己 scp）
  *         - 输入 224x224 RGB888，输出 7x7 grid，5 anchors
  *         - 量化 mean = {127.5, 127.5, 127.5}, norm = {0.0078125, 0.0078125, 0.0078125}
  *    2. 数据通路（V831 双 cam，CPU 不再做色彩空间/resize/拷贝）：
  *         - cam0：640×480 NV21 → 显示 + H264 推流（由 main 主循环负责）
  *         - cam1：224×224 RGB888 → 直接喂给 NPU 推理
  *         cam1 必须由调用方（main）在 cam0 创建并 start_capture 之后立即
  *         create + start_capture，再通过 SetAiCam() 注入本模块。本模块仅"使用"
  *         该 cam（capture_image → 零拷贝喂 forward），不 own、不 destroy。
  *         之所以放外部创建是因为 V831 ISP 对 cam0/cam1 创建顺序非常敏感，
  *         必须连续创建且与显示/编码模块构造解耦，否则 cam0 输出绿屏。
  *    3. 触发逻辑：
  *         - 监听 AppConfig.ai_enabled；false → 推理线程仅 drain cam1 防 vipp 缓冲
  *           堆积，不做 forward / 不加载模型
  *         - 每次推理拿到 boxes_num > 0 时：检查 (now - lastTriggerMs) >= ai_min_interval_s*1000
  *           满足才调 m_pSnapshot->TakeOne()；不满足则忽略，避免狂闪
  *    4. 模型文件缺失的容错：Init 找不到模型文件直接返回 -1，不影响其他模块；
  *         AppConfig 仍可正常切换 ai_enabled，只会在每次重启时重新尝试加载。
  *    5. cam1 生命周期：由 main 在 cam0 之后立即 create + start_capture，并通过
  *         SetAiCam() 注入本模块；进程退出时由 main 统一 destroy。本模块只用、不持有。
  *         之所以放外部创建是为了规避 V831 ISP 早期初始化阶段对 cam0/cam1 创建顺序
  *         的敏感性（参考老代码 main.cpp："不创建会有莫名其表的bug"）。
**********************************************************************************/
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class C_Snapshot;
struct libmaix_nn;
typedef struct libmaix_nn libmaix_nn_t;
struct libmaix_nn_decoder;
typedef struct libmaix_nn_decoder libmaix_nn_decoder_t;
struct libmaix_cam;
typedef struct libmaix_cam libmaix_cam_t;
struct libmaix_image;
typedef struct libmaix_image libmaix_image_t;

class C_PersonDetector
{
public:
    // width/height：兼容老接口的入参（已不再使用，cam1 固定 224×224 RGB888）
    C_PersonDetector(int width, int height);
    ~C_PersonDetector();

    // 绑定 Snapshot 模块（用于触发拍照）
    void SetSnapshot(C_Snapshot* snap) { m_pSnapshot = snap; }

    // 注入由外部（main）创建并已 start_capture 的 cam1（224×224 RGB888）。
    // 必须在 Start() 之前调用。本模块只"使用"该 cam，不 own、不 destroy。
    void SetAiCam(libmaix_cam_t* cam) { m_aiCam = cam; }

    // 启动后台推理线程。modelDir：模型所在目录，例如 "/root/models"
    // 返回 0 成功；非 0 失败。
    // 前置条件：调用方已 SetAiCam() 注入有效 cam1（V831 ISP 顺序约束）。
    int Start(const std::string& modelDir);

    // Rust host mode: keep NPU/cam1 inference native, but let the host own
    // enable/fps/threshold policy and snapshot cooldown/dispatch.
    void SetExternalConfig(bool enabled, float threshold, int inferFps);
    void SetDetectionCallback(std::function<void(float)> callback);

    // 停止后台线程，释放模型 + cam1 资源。可重入。
    void Stop();

    // 是否已成功加载模型 & 线程已起来
    bool IsReady() const { return m_ready.load(); }

    // ---- 检测框结果（供 main 主循环在 cam0 NV21 上叠加绘制）----
    // 单个 box，坐标已归一化到 [0,1]：(x_center, y_center, w, h) —— 直接对应 yolo2 的输出语义。
    // 调用方按目标分辨率乘 W/H 即可得到像素坐标。
    struct Box {
        float xc;      // 中心 x（归一化）
        float yc;
        float w;
        float h;
        float prob;    // 置信度
    };
    // 取最新一帧的检测结果（线程安全）。
    // maxAgeMs：仅当 (now - lastInferMs) <= maxAgeMs 时才返回；否则返回空 vector。
    //          这样在低 ai_infer_fps（如 1Hz）下不会让画面长期 "粘" 着旧框。
    std::vector<Box> GetLatestBoxes(int maxAgeMs = 1000) const;

private:
    struct RuntimeConfig {
        bool enabled;
        float threshold;
        int inferFps;
    };
    RuntimeConfig GetRuntimeConfig() const;

    void RunLoop();             // 后台线程入口
    bool DoInferOnce();         // 取 cam1 一帧 → 推理 → 解码 → 触发拍照；true=有 box
    bool DrainOnce();           // AI 关时仅消费一帧 cam1（防 vipp 堆积）
    bool LoadModel();           // 创建/加载 NN + decoder（失败返回 false）
    void UnloadModel();         // 释放 NN + decoder

private:
    int          m_width;
    int          m_height;
    std::string  m_modelDir;

    C_Snapshot*  m_pSnapshot = nullptr;

    // ---- AI 专用 cam（cam1，HWC RGB888 224×224）----
    // 由外部 SetAiCam() 注入，本类不 own，不在 Stop() 中 destroy。
    libmaix_cam_t*              m_aiCam = nullptr;
    libmaix_image_t*            m_aiImage = nullptr;     // capture_image 输出，driver 内部循环 buffer

    // ---- NN ----
    libmaix_nn_t*               m_nn = nullptr;
    libmaix_nn_decoder_t*       m_decoder = nullptr;
    void*                       m_yoloCfg = nullptr;     // libmaix_nn_decoder_yolo2_config_t*
    std::vector<float>          m_anchors;               // 5 对 = 10 个 float
    std::vector<float>          m_outBuf;                // 网络输出 fmap
    std::vector<unsigned char>  m_quantBuf;              // 量化临时缓冲（forward 内部写）

    // ---- 线程 ----
    std::thread                 m_thread;
    std::atomic<bool>           m_running{false};
    std::atomic<bool>           m_ready  {false};
    bool                        m_modelLoaded = false;   // 懒加载标志：仅由 RunLoop 线程读写
    std::atomic<bool>           m_externalConfig{false};
    std::atomic<bool>           m_externalEnabled{false};
    std::atomic<float>          m_externalThreshold{0.6f};
    std::atomic<int>            m_externalInferFps{5};
    std::function<void(float)>  m_detectionCallback;

    // ---- 触发节流 ----
    int64_t                     m_lastTriggerMs = 0;

    // ---- 给主循环做"叠加绘制"用的最新结果（独立于触发节流）----
    // 写入：推理线程；读取：main 主循环（30Hz）。简单 mutex 即可，box 数量极少（几十）。
    mutable std::mutex          m_boxMu;
    std::vector<Box>            m_latestBoxes;
    int64_t                     m_lastInferMs = 0;
};
