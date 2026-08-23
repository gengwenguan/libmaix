/*********************************************************************************
  *Copyright(C),Your Company
  *FileName:  personDetector.cpp
  *Author:    gengwenguan
  *Date:      2026-05-23
  *Description:  人形识别 → 自动拍照实现。详见 personDetector.h 顶部注释。
**********************************************************************************/
#include "personDetector.h"
#include "appConfig.h"
#ifndef CAMERA_RUST_HOST
#include "snapshot.h"
#endif
#include "logAdapt.h"

extern "C" {
#include "libmaix_nn.h"
#include "libmaix_nn_decoder.h"
#include "libmaix_nn_decoder_yolo2.h"
#include "libmaix_cam.h"
#include "libmaix_image.h"
}

#include <chrono>
#include <cstring>
#include <sys/stat.h>
#include <utility>

namespace {
constexpr uint32_t kNetIn  = 224;
constexpr uint32_t kNetOut = 7;
constexpr uint32_t kClassNum  = 1;
constexpr uint32_t kAnchorNum = 5;

inline int64_t NowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

inline bool FileExists(const std::string& path)
{
    struct stat st{};
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}
} // namespace

C_PersonDetector::C_PersonDetector(int width, int height)
    : m_width(width), m_height(height)
{
    // 推理用临时 buffer（forward 内部写量化结果 / decoder 写 fmap）
    // 注意：不再缓存 RGB 帧 —— 直接用 cam1 capture_image 返回的 driver buffer
    m_quantBuf.assign((size_t)kNetIn * kNetIn * 3, 0);
    m_outBuf.assign((size_t)kNetOut * kNetOut * (kClassNum + 5) * kAnchorNum, 0.0f);

    // YOLOv2 anchors（与 nn_yolo_person/main.c 完全一致）
    m_anchors = { 4.72f, 6.26f, 1.39f, 3.53f, 0.78f, 1.9f, 0.35f, 0.95f, 2.49f, 4.87f };
}

C_PersonDetector::~C_PersonDetector()
{
    Stop();
}

void C_PersonDetector::SetExternalConfig(bool enabled, float threshold, int inferFps)
{
    m_externalEnabled.store(enabled);
    m_externalThreshold.store(threshold);
    m_externalInferFps.store(inferFps);
    m_externalConfig.store(true);
}

void C_PersonDetector::SetDetectionCallback(std::function<void(float)> callback)
{
    m_detectionCallback = std::move(callback);
}

C_PersonDetector::RuntimeConfig C_PersonDetector::GetRuntimeConfig() const
{
    if (m_externalConfig.load()) {
        return RuntimeConfig{
            m_externalEnabled.load(),
            m_externalThreshold.load(),
            m_externalInferFps.load(),
        };
    }
    const auto config = C_AppConfig::GetInst().GetSnapshot();
    return RuntimeConfig{config.ai_enabled, config.ai_threshold, config.ai_infer_fps};
}

int C_PersonDetector::Start(const std::string& modelDir)
{
    if (m_running.load()) {
        CLOG_INF("personDetector: already running\n");
        return 0;
    }
    m_modelDir = modelDir;

    std::string binPath   = m_modelDir + "/person_int8.bin";
    std::string paramPath = m_modelDir + "/person_int8.param";
    if (!FileExists(binPath) || !FileExists(paramPath)) {
        CLOG_ERR("personDetector: model files missing under %s\n", m_modelDir.c_str());
        CLOG_ERR("  expected: person_int8.bin / person_int8.param\n");
        return -1;
    }

    // cam1 由外部（main）创建并 start_capture 之后通过 SetAiCam() 注入，
    // 本类不再自行 create —— 否则 V831 ISP 因 cam0/cam1 创建间穿插了过多模块
    // 构造（vo / Terminal / TLS / HTTP API …），会导致 cam0 输出绿屏。
    if (!m_aiCam) {
        CLOG_ERR("personDetector: ai cam not set, call SetAiCam() before Start()\n");
        return -2;
    }

    // 懒加载：先不真加载模型（~12MB 占用），只启动调度线程；
    // 当 web 上把 ai_enabled 切到 true 时再 LoadModel，避免 60MB 内存的开发板 OOM。
    m_running.store(true);
    m_thread = std::thread(&C_PersonDetector::RunLoop, this);
    m_ready.store(true);
    CLOG_INF("personDetector: thread started (lazy load, model dir=%s)\n", m_modelDir.c_str());
    return 0;
}

void C_PersonDetector::Stop()
{
    if (!m_running.exchange(false)) return;
    if (m_thread.joinable()) m_thread.join();
    if (m_modelLoaded) {
        UnloadModel();
        m_modelLoaded = false;
    }
    // cam1 由外部 own，本类不 destroy；只清空指针，避免悬挂引用。
    m_aiCam = nullptr;
    m_aiImage = nullptr;
    m_ready.store(false);
    CLOG_INF("personDetector: stopped\n");
}

// --------------------------------------------------------------------
// 模型加载 / 释放
// --------------------------------------------------------------------
bool C_PersonDetector::LoadModel()
{
    libmaix_nn_module_init();

    // YOLOv2 decoder 配置（输入图等于网络输入，避免再缩 box 坐标）
    auto* cfg = new libmaix_nn_decoder_yolo2_config_t();
    cfg->classes_num    = kClassNum;
    cfg->threshold      = GetRuntimeConfig().threshold;
    cfg->nms_value      = 0.5f;
    cfg->anchors_num    = kAnchorNum;
    cfg->anchors        = m_anchors.data();
    cfg->net_in_width   = kNetIn;
    cfg->net_in_height  = kNetIn;
    cfg->net_out_width  = kNetOut;
    cfg->net_out_height = kNetOut;
    cfg->input_width    = kNetIn;
    cfg->input_height   = kNetIn;
    m_yoloCfg = cfg;

    // create + init nn
    m_nn = libmaix_nn_create();
    if (!m_nn) {
        CLOG_ERR("personDetector: libmaix_nn_create failed\n");
        return false;
    }
    if (m_nn->init(m_nn) != LIBMAIX_ERR_NONE) {
        CLOG_ERR("personDetector: nn->init failed\n");
        return false;
    }

    // load awnn model（V831）
    static char binPath[256]   = {0};
    static char paramPath[256] = {0};
    std::snprintf(binPath,   sizeof(binPath),   "%s/person_int8.bin",   m_modelDir.c_str());
    std::snprintf(paramPath, sizeof(paramPath), "%s/person_int8.param", m_modelDir.c_str());

    libmaix_nn_model_path_t modelPath{};
    modelPath.awnn.bin_path   = binPath;
    modelPath.awnn.param_path = paramPath;

    static char* inputNames[]  = { (char*)"input0"  };
    static char* outputNames[] = { (char*)"output0" };

    libmaix_nn_opt_param_t optParam{};
    optParam.awnn.input_names  = inputNames;
    optParam.awnn.output_names = outputNames;
    optParam.awnn.input_num    = 1;
    optParam.awnn.output_num   = 1;
    optParam.awnn.mean[0] = 127.5f; optParam.awnn.mean[1] = 127.5f; optParam.awnn.mean[2] = 127.5f;
    optParam.awnn.norm[0] = 0.0078125f; optParam.awnn.norm[1] = 0.0078125f; optParam.awnn.norm[2] = 0.0078125f;

    if (m_nn->load(m_nn, &modelPath, &optParam) != LIBMAIX_ERR_NONE) {
        CLOG_ERR("personDetector: nn->load failed\n");
        return false;
    }

    // decoder
    m_decoder = libmaix_nn_decoder_yolo2_create(libmaix_nn_decoder_yolo2_init,
                                                libmaix_nn_decoder_yolo2_deinit,
                                                libmaix_nn_decoder_yolo2_decode);
    if (!m_decoder) {
        CLOG_ERR("personDetector: decoder create failed\n");
        return false;
    }
    if (m_decoder->init(m_decoder, m_yoloCfg) != LIBMAIX_ERR_NONE) {
        CLOG_ERR("personDetector: decoder init failed\n");
        return false;
    }
    return true;
}

void C_PersonDetector::UnloadModel()
{
    if (m_decoder) {
        m_decoder->deinit(m_decoder);
        libmaix_nn_decoder_yolo2_destroy(&m_decoder);
        m_decoder = nullptr;
    }
    if (m_nn) {
        libmaix_nn_destroy(&m_nn);
        m_nn = nullptr;
    }
    if (m_yoloCfg) {
        delete static_cast<libmaix_nn_decoder_yolo2_config_t*>(m_yoloCfg);
        m_yoloCfg = nullptr;
    }
    libmaix_nn_module_deinit();
}

// --------------------------------------------------------------------
// 单次推理 + 触发判定
//   - cam1 直接输出 224×224 RGB888 HWC，整个数据通路零拷贝：
//     driver 循环 buffer (m_aiImage->data) → input.data → forward
//   - 没有任何锁竞争（cam1 完全由本模块独占）
// --------------------------------------------------------------------
bool C_PersonDetector::DoInferOnce()
{
    if (!m_aiCam) return false;
    auto cfg = GetRuntimeConfig();

    // 1. 从 cam1 拿一帧 RGB888（driver 内部循环 buffer，地址直接复用，无拷贝）
    libmaix_err_t cerr = m_aiCam->capture_image(m_aiCam, &m_aiImage);
    if (cerr != LIBMAIX_ERR_NONE) {
        // LIBMAIX_ERR_NOT_READY：cam1 还没出帧，下个周期再来
        return false;
    }
    if (!m_aiImage || !m_aiImage->data) return false;

    // 2. 同步 threshold 到 decoder（用户在 web 上调阈值时无需重启线程）
    if (m_yoloCfg) {
        auto* yc = static_cast<libmaix_nn_decoder_yolo2_config_t*>(m_yoloCfg);
        yc->threshold = cfg.threshold;
    }

    // 3. forward —— 直接把 driver buffer 当 input.data，零拷贝
    libmaix_nn_layer_t input{};
    input.w = kNetIn; input.h = kNetIn; input.c = 3;
    input.dtype = LIBMAIX_NN_DTYPE_UINT8;
    input.data = (uint8_t*)m_aiImage->data;
    input.need_quantization = true;
    input.buff_quantization = m_quantBuf.data();

    libmaix_nn_layer_t outFmap{};
    outFmap.w = kNetOut; outFmap.h = kNetOut;
    outFmap.c = (kClassNum + 5) * kAnchorNum;
    outFmap.dtype = LIBMAIX_NN_DTYPE_FLOAT;
    outFmap.data = m_outBuf.data();

    libmaix_err_t err = m_nn->forward(m_nn, &input, &outFmap);
    if (err != LIBMAIX_ERR_NONE) {
        CLOG_ERR("personDetector: forward failed err=%d\n", (int)err);
        return false;
    }

    // 4. decode
    libmaix_nn_decoder_yolo2_result_t result{};
    err = m_decoder->decode(m_decoder, &outFmap, (void*)&result);
    if (err != LIBMAIX_ERR_NONE) {
        CLOG_ERR("personDetector: decode failed err=%d\n", (int)err);
        return false;
    }

    if (result.boxes_num == 0) {
        // 没有任何候选 → 清空"最新框"，让主循环停止画框
        std::lock_guard<std::mutex> lk(m_boxMu);
        m_latestBoxes.clear();
        m_lastInferMs = NowMs();
        return false;
    }

    // 4.1 自己过滤 threshold —— decoder 会把所有 anchor 全返回，
    //     需要使用方按 prob 与 threshold 比较（参见 decoder_yolo2.c:draw_result）
    //     同时把所有"过阈值"的框都收集起来，供 main 在 cam0 上叠加绘制
    std::vector<Box> hits;
    hits.reserve(result.boxes_num);
    int   hitIdx   = -1;
    float maxProb  = 0.f;
    for (uint32_t i = 0; i < result.boxes_num; ++i) {
        // class_num=1，class_id 固定 0
        float p = result.probs[i][0];
        if (p < cfg.threshold) continue;
        const auto& b = result.boxes[i];
        // yolo2 输出已经归一化到 [0,1]（中心点 + 宽高，相对于 input_width/height）
        hits.push_back(Box{ b.x, b.y, b.w, b.h, p });
        if (p > maxProb) { maxProb = p; hitIdx = (int)i; }
    }

    // 把当前帧的检测结果发布给主循环（即使为空也要刷新时间戳，避免画面留旧框）
    {
        std::lock_guard<std::mutex> lk(m_boxMu);
        m_latestBoxes = std::move(hits);
        m_lastInferMs = NowMs();
    }

    if (hitIdx < 0) {
        return false;
    }

    if (m_externalConfig.load()) {
        if (m_detectionCallback) m_detectionCallback(maxProb);
        return true;
    }

    // 5. 触发节流
    auto appConfig = C_AppConfig::GetInst().GetSnapshot();
    int64_t now = NowMs();
    int64_t minIntervalMs = (int64_t)appConfig.ai_min_interval_s * 1000;
    if (m_lastTriggerMs != 0 && (now - m_lastTriggerMs) < minIntervalMs) {
        return true;  // 检测到了人，但还在冷却期
    }
    m_lastTriggerMs = now;

#ifndef CAMERA_RUST_HOST
    if (m_pSnapshot) {
        std::string name = m_pSnapshot->TakeOne();
        CLOG_INF("personDetector: person detected (prob=%.2f thresh=%.2f), snapshot -> %s\n",
                 maxProb, cfg.threshold, name.c_str());
    } else {
        CLOG_INF("personDetector: person detected (prob=%.2f) but no snapshot\n",
                 maxProb);
    }
#endif
    return true;
}

// --------------------------------------------------------------------
// AI 关时仍需消费 cam1 的帧，否则 vipp[1] 缓冲会堆积报告
//   "frames are not release / video1 select timeout"。
// 这里只是把 driver buffer "认领并丢弃"，不做任何后续处理，开销极小。
// --------------------------------------------------------------------
bool C_PersonDetector::DrainOnce()
{
    if (!m_aiCam) return false;
    libmaix_err_t cerr = m_aiCam->capture_image(m_aiCam, &m_aiImage);
    // AI 关时即便 cam1 仍在跑，也不应该让旧框继续叠加；这里清掉缓存。
    {
        std::lock_guard<std::mutex> lk(m_boxMu);
        if (!m_latestBoxes.empty()) m_latestBoxes.clear();
    }
    return cerr == LIBMAIX_ERR_NONE;
}

std::vector<C_PersonDetector::Box> C_PersonDetector::GetLatestBoxes(int maxAgeMs) const
{
    std::lock_guard<std::mutex> lk(m_boxMu);
    if (m_latestBoxes.empty()) return {};
    int64_t now = NowMs();
    if (m_lastInferMs == 0 || (now - m_lastInferMs) > maxAgeMs) {
        return {};   // 推理已停止 / 帧过期
    }
    return m_latestBoxes;   // 浅拷贝（vector of POD）
}

// --------------------------------------------------------------------
// 后台线程：
//   - ai_enabled=true：按 ai_infer_fps 节奏推理；间隙 sleep 让出 CPU
//   - ai_enabled=false：以低频（~5 Hz）drain cam1 防 vipp 堆积；持续 5 秒后释放模型
// --------------------------------------------------------------------
void C_PersonDetector::RunLoop()
{
    CLOG_INF("personDetector: thread loop start\n");
    int64_t disabledSinceMs = 0;
    while (m_running.load()) {
        auto cfg = GetRuntimeConfig();

        if (!cfg.enabled) {
            if (disabledSinceMs == 0) disabledSinceMs = NowMs();
            if (m_modelLoaded && NowMs() - disabledSinceMs >= 5000) {
                UnloadModel();
                m_modelLoaded = false;
                CLOG_INF("personDetector: disabled, model unloaded\n");
            }
            // 关闭状态：仍要 drain cam1，但 5 Hz 足够（一帧 200ms 不会堆积）
            DrainOnce();
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }
        disabledSinceMs = 0;

        // 懒加载：第一次开启时才真的把 ~12MB 模型加载进来
        if (!m_modelLoaded) {
            CLOG_INF("personDetector: ai_enabled=true, loading model now ...\n");
            if (!LoadModel()) {
                CLOG_ERR("personDetector: LoadModel failed, will retry in 5s\n");
                UnloadModel();
                std::this_thread::sleep_for(std::chrono::seconds(5));
                continue;
            }
            m_modelLoaded = true;
            CLOG_INF("personDetector: model loaded\n");
        }

        int fps = cfg.inferFps;
        if (fps < 1) fps = 1;
        if (fps > 30) fps = 30;
        int periodMs = 1000 / fps;

        int64_t t0 = NowMs();
        DoInferOnce();
        int64_t cost = NowMs() - t0;
        int sleepMs = periodMs - (int)cost;
        if (sleepMs > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
        }
    }
    CLOG_INF("personDetector: thread loop exit\n");
}
