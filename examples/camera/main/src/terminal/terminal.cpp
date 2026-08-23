#include<iostream>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>  // 包含这个头文件以确保 NI_MAXHOST 和 NI_NUMERICHOST 定义
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <algorithm>
#include <cctype>
#include <sstream>
#include <fstream>
#include <cstring>
#include <tuple>
#include <vector>
#include"logAdapt.h"
#include "terminal.h"
#include "liveHub.h"
#include "appConfig.h"
#include "httpClient.h"

// ----------------------------------------------------------------------
// 给 raw AAC 加 ADTS 头（7 字节，无 CRC）：浏览器侧 Web Audio API 在
// decodeAudioData 时只识别带容器的 AAC（ADTS / mp4）；纯 raw AAC 无法解。
// 这里复用 aacEnc.cpp 内部 dump 路径相同的算法，但单独提一份避免循环依赖。
// ----------------------------------------------------------------------
namespace {
inline int AscFreqIndex(int sampleRate)
{
    static const int kFreqTable[] = {
        96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050,
        16000, 12000, 11025, 8000,  7350,  0,     0,     0
    };
    for (int i = 0; i < 13; ++i) {
        if (kFreqTable[i] == sampleRate) return i;
    }
    return 3;  // 默认 48000Hz
}

// 把 raw AAC 加上 ADTS 头放进 outBuf；outBuf 长度 = 7 + aacLen
void BuildAdtsFrame(const unsigned char* aac, unsigned int aacLen,
                    int sampleRate, int channels,
                    std::vector<unsigned char>& outBuf)
{
    const int profile = 1;            // AAC-LC = aot 2 - 1
    const int chCfg   = channels;
    const int freqIdx = AscFreqIndex(sampleRate);
    const unsigned int frameLen = 7 + aacLen;

    outBuf.resize(frameLen);
    unsigned char* p = outBuf.data();
    p[0] = 0xFF;
    p[1] = 0xF1;                                            // MPEG-4 + protection_absent
    p[2] = (profile << 6) | (freqIdx << 2) | (chCfg >> 2);
    p[3] = ((chCfg & 3) << 6) | ((frameLen >> 11) & 0x03);
    p[4] = (frameLen >> 3) & 0xFF;
    p[5] = ((frameLen & 0x07) << 5) | 0x1F;
    p[6] = 0xFC;
    memcpy(p + 7, aac, aacLen);
}
} // namespace


C_Terminal::C_Terminal(unsigned int Wight, unsigned int Hight, libmaix_cam_t* aiCam)
    :m_Wight(Wight),
    m_Hight(Hight),
    m_pNv12Buff(new unsigned char[Wight*Hight+Wight*Hight/2]),
    // WebSocket 直播：8081，订阅 LiveHub 接收 fMP4
    m_pWsServer(new C_WebSocketServer(this, 8081, true)),
    // WebSocket 回放：8082（保留旧文件回放路径）
    m_pWsFileServer(new C_WebSocketServer(this, 8082, false)),
    m_pHttpServer(new C_HttpServer(80)),
    m_pH264Enc(new C_H264Enc(this, Wight, Hight, Wight, Hight)),
    m_pAacEnc(new C_AacEnc(this)),
    m_pRecorder(new C_RollingRecorder()),
    m_pCleaner(new C_RecordCleaner()),
    m_pSnapshot(new C_Snapshot((int)Wight, (int)Hight)),
    m_pPersonDetector(new C_PersonDetector((int)Wight, (int)Hight)),
    m_pVmd(new C_MotionDetector((int)Wight, (int)Hight))
{
    CLOG_INF("Terminal初始化完成\n");
    CLOG_INF("WebSocket直播端口: 8081 (fMP4 over WebSocket)\n");
    CLOG_INF("WebSocket回放端口: 8082\n");
    CLOG_INF("HTTP服务器端口: 80\n");

    // 录像目录：放在 <exe_dir>/record/，跟 web/ 同级，方便统一管理
    char exePath[1024] = {0};
    ssize_t n = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
    std::string exeDir;
    if (n > 0) {
        std::string ep(exePath);
        size_t slash = ep.find_last_of('/');
        exeDir = (slash != std::string::npos ? ep.substr(0, slash) : ".");
    } else {
        exeDir = ".";
    }
    m_recordDir   = exeDir + "/record";
    m_snapshotDir = exeDir + "/snapshot";
    m_promptDir   = exeDir + "/prompt";
    m_actionsPath = exeDir + "/actions.json";
    m_acmeChallengeDir = exeDir + "/state/acme-webroot/.well-known/acme-challenge";
    mkdir(m_recordDir.c_str(),   0755);
    mkdir(m_snapshotDir.c_str(), 0755);
    // m_promptDir 由 CMake 在 dist/prompt/ 下生成 wav；运行时不创建，
    // 缺失时 PlayWavSync 会按 -1（文件不存在）处理。
    CLOG_INF("录像目录: %s\n", m_recordDir.c_str());
    CLOG_INF("拍照目录: %s\n", m_snapshotDir.c_str());

    // 设备动作代理配置：加载 <exeDir>/actions.json（不存在则空列表）。
    m_pActionStore.reset(new C_ActionStore());
    m_pActionStore->Init(m_actionsPath);

    // 注册 HTTP API：必须在 HttpServer Start() 之前/之后均可，路由表是独立的
    RegisterHttpApis();

    // ---- TLS / HTTPS / wss ----
    // 权威证书由 Camera-hub ACME 管理器原子同步到共享 state/tls；
    // 旧 cert/ 只在首次迁移尚未签发时回退使用。
    // 注意：HTTP/WS 始终保留；HTTPS/WSS 只是"加一组监听端口"。
    // 若证书加载失败，程序仍可以通过 80 / 8081 / 8082 提供明文服务，
    // 仅前端"讲话"按钮会因 location.protocol !== 'https:' 而隐藏。
    // 必须在 m_pHttpServer->Start() 之前 EnableTls，HttpServer 的 Start()
    // 会同时 bind/listen TLS 端口；WebSocketServer 的 AcceptThread 是循环检查
    // m_tlsServerFd，因此构造之后再 EnableTls 也能即时生效。
    {
        std::string crt = exeDir + "/state/tls/fullchain.pem";
        std::string key = exeDir + "/state/tls/private.key";
        if (access(crt.c_str(), R_OK) != 0 || access(key.c_str(), R_OK) != 0) {
            crt = exeDir + "/cert/server.crt";
            key = exeDir + "/cert/server.key";
        }
        std::unique_ptr<C_TlsContext> tls(new C_TlsContext());
        if (tls->Init(crt, key)) {
            m_pTls = std::move(tls);
            m_pHttpServer  ->EnableTls(443, m_pTls.get());
            m_pWsServer    ->EnableTls(8444, m_pTls.get());
            m_pWsFileServer->EnableTls(8445, m_pTls.get());
            CLOG_INF("HTTPS=443  WSS-live=8444  WSS-playback=8445  cert=%s\n",
                     crt.c_str());
        } else {
            CLOG_INF("未找到 TLS 证书 (%s)，仅启用 HTTP/WS 明文模式；"
                     "讲话按钮在 HTTP 上不可用\n", crt.c_str());
        }
    }

    // 监控形态：进程启动即开始录像，按片滚动 + 按天分目录
    // 双兜底：保留 N 天 + 总容量上限。两个阈值都由 AppConfig 提供，
    // Recorder/Cleaner 内部按需现拉，不再保存副本，因此 web 改完即时生效（Recorder
    // 下个 fragment 边界，Cleaner 下个扫描周期）。
    m_pRecorder->Start(m_recordDir);
    m_pCleaner ->Start(m_recordDir, /*intervalSec=*/3600);

    // 拍照模块（独立目录；自动 prune 由 AppConfig.album_max_photos 控制）
    m_pSnapshot->Start(m_snapshotDir);

    // 人形识别模块。模型放在 /root/models/，缺失时 Start 失败但不影响其它功能。
    // 即使 ai_enabled=false，也先把线程拉起来；线程内部按 AppConfig 决定是否真正推理。
    // cam1 必须由 main 在 cam0 之后立即 create + start_capture，再传进来（V831 ISP 顺序约束）。
    m_pPersonDetector->SetSnapshot(m_pSnapshot.get());
    if (aiCam) {
        m_pPersonDetector->SetAiCam(aiCam);
        if (m_pPersonDetector->Start("/root/models") != 0) {
            CLOG_INF("personDetector 未就绪：请把 awnn_yolo_person.{bin,param} 放到 /root/models/\n");
        }
    } else {
        CLOG_INF("personDetector 跳过启动：未注入 cam1（main 未传 aiCam）\n");
    }

    // ---- 讲话播放器（浏览器 OPUS → 开发板扬声器）----
    // 单 ALSA 设备 + 单线程解码，构造里就打开 PCM；若失败仍可正常运行不影响其他功能。
    m_pTalkPlayer.reset(new C_TalkPlayer());

    // ---- 移动侦测（VMD）----
    // 与 PersonDetector 平行：PersonDetector 用 NPU + AI 模型识别"人形"；
    // VMD 用纯 CPU 帧差判定"画面是否在变化"，不识别物体类型。
    // 二者共用 Snapshot 模块；具体启用与否由 AppConfig.vmd_enabled / ai_enabled 各自控制。
    // 这里始终 Start，线程内会按 vmd_enabled 走极速 fast-path（一次 atomic load 即返回）。
    m_pVmd->SetSnapshot(m_pSnapshot.get());
    m_pVmd->Start();

    // ---- IPv6 地址变化 MQTT 上报 ----
    // 常驻后台线程，定时轮询 wlan0 全局 IPv6，变化时通过 MQTT 上报。
    // 始终 Start；线程内按 AppConfig.mqtt_enabled 决定是否真正采集/连网（默认关，空转）。
    m_pMqttReporter.reset(new C_MqttReporter());
    m_pMqttReporter->Start();

    // ---- 日志广播器 ----
    // 只创建对象，不启动线程、不注册 sink（禁止构造函数内发布 this）。
    // 真正的 Start/Stop 与 sink 注册在 C_Terminal::Start()/Stop() 里完成。
    m_pLogBroadcaster.reset(new C_LogBroadcaster());

    // ---- 系统信息采集器 ----
    // 无线程、无副作用，仅在 /api/sysinfo 被调用时读 procfs；构造即可用。
    m_pSysInfo.reset(new C_SysInfoProvider());

    // ---- 外接补光灯控制器 ----
    // 只创建对象；线程在 C_Terminal::Start() 里拉起（析构反序在 Stop() 里停）。
    m_pLight.reset(new C_LightController());
}


C_Terminal::~C_Terminal(){
    Stop();
}

int C_Terminal::Start()
{
    bool expected = false;
    if (!m_started.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return 0;
    }

    // AAC 会回调 OnOutputAac，网络服务会回调 listener；必须等 C_Terminal
    // 完整构造后再启动，禁止构造函数内发布 this。
    if (!m_pWsServer || m_pWsServer->Start() != 0 ||
        !m_pWsFileServer || m_pWsFileServer->Start() != 0 ||
        !m_pHttpServer || m_pHttpServer->Start() != 0 ||
        !m_pAacEnc || m_pAacEnc->Start() != 0) {
        CLOG_ERR("Terminal服务启动失败，正在回滚\n");
        Stop();
        return -1;
    }

    // 服务全部就绪后再启动日志广播：推送线程把日志经直播 WS 发给 /ws/log 订阅者。
    // 此时 m_pWsServer 已 Start，回调安全。注册 sink 后，全局 CLOG_* 才开始分叉。
    if (m_pLogBroadcaster) {
        C_WebSocketServer* ws = m_pWsServer.get();
        m_pLogBroadcaster->Start([ws](const char* d, size_t n) {
            if (ws) ws->BroadcastLogText(d, n);
        });
        SetLogSink(m_pLogBroadcaster.get());
    }

    // 补光灯控制器：常驻线程按 AppConfig.light_* 评估点亮。始终启动，
    // 未启用时线程空转并保证灯灭；声控模式读 AAC 采集线程发布的响度原子量。
    if (m_pLight) m_pLight->Start();

    CLOG_INF("Terminal服务已全部启动\n");
    return 0;
}

void C_Terminal::Stop()
{
    m_started.store(false, std::memory_order_release);

    // 最先注销日志 sink：此后 CLOG_* 不再分叉进广播队列，且 logAdapt 不再持有
    // 广播器指针。推送线程稍后单独停。避免"WS 已停但仍被日志回调触及"。
    SetLogSink(nullptr);

    // 先停止所有外部入口并等待客户端线程退出。此时它们依赖的 TLS、编码器、
    // Recorder、Snapshot、TalkPlayer 均仍存活。
    if (m_pHttpServer)   m_pHttpServer->Stop();
    if (m_pWsFileServer) m_pWsFileServer->Stop();
    if (m_pWsServer)    m_pWsServer->Stop();

    // WS 已停（不再有 BroadcastLogText 触及），此时安全 join 推送线程。
    if (m_pLogBroadcaster) m_pLogBroadcaster->Stop();
    {
        std::lock_guard<std::mutex> lk(m_logFdsMutex);
        m_logFds.clear();
    }

    // 停止最后一个会并发访问 muxer/WS 的生产者，再释放 muxer。
    if (m_pAacEnc) m_pAacEnc->Stop();

    // 补光灯：停线程前会先灭灯并释放 GPIO（见 C_LightController::Stop）。
    // 放在 AAC 之后即可——它只读 AAC 的响度原子量，AAC 停后响度已归零。
    if (m_pLight) m_pLight->Stop();

    if (m_pMqttReporter) m_pMqttReporter->Stop();
    if (m_pVmd) m_pVmd->Stop();
    if (m_pPersonDetector) m_pPersonDetector->Stop();
    if (m_pTalkPlayer) m_pTalkPlayer->Shutdown();
    {
        std::lock_guard<std::mutex> lk(m_talkFdsMutex);
        m_talkFds.clear();
    }

    if (m_pCleaner) m_pCleaner->Stop();
    {
        std::lock_guard<std::mutex> lk(m_muxerMutex);
        // 析构会 flush 最后一个 fragment，Recorder 此时仍保持订阅。
        m_pMuxer.reset();
    }
    if (m_pRecorder) m_pRecorder->Stop();
}

//送入采集数据
int C_Terminal::InputRgb888(unsigned char* inputData)
{
    rgb888ToNv21(inputData, m_pNv12Buff.get(), m_Wight, m_Hight);

    return InputNv21(m_pNv12Buff.get());
}

//送入采集数据
int C_Terminal::InputNv21(unsigned char* inputData)
{
    // 同步给拍照模块的"快路径"：snapshot 内部用 atomic 标志判定是否真要 memcpy；
    // 没有拍照请求挂起时这里只是一次原子 load，几乎零开销（不再每帧 memcpy 460KB）。
    // 注意：人形识别现在由 PersonDetector 内部直接管理 cam1（224×224 RGB888），
    //       走零拷贝路径，main/terminal 都不再感知 AI cam。
    if (m_pSnapshot) m_pSnapshot->OnNv21Frame(inputData);
    // VMD：相机回调线程同步调，关闭时仅一次 atomic load 立即返回，开销可忽略
    if (m_pVmd) m_pVmd->InputNv21(inputData);
    return m_pH264Enc->InputData(inputData);
}

// 转发给 PersonDetector：main 主循环每 30Hz 拉一次，AI 推理实际只 1~10Hz；
// 推理线程每帧推理后会刷新缓存，1s 没新结果就返回空，避免画面留旧框。
std::vector<C_PersonDetector::Box> C_Terminal::GetLatestAiBoxes(int maxAgeMs) const
{
    if (!m_pPersonDetector) return {};
    return m_pPersonDetector->GetLatestBoxes(maxAgeMs);
}

std::shared_ptr<C_Fmp4Muxer> C_Terminal::GetOrCreateMuxer()
{
    std::lock_guard<std::mutex> lk(m_muxerMutex);
    if (m_pMuxer) return m_pMuxer;

    unsigned int spsPpsLen = 0;
    const unsigned char* spsPps = m_pH264Enc->GetSpsPps(&spsPpsLen);
    if (!spsPps || spsPpsLen == 0) {
        // SPS/PPS 还没准备好，等下一帧
        return nullptr;
    }

    unsigned int ascLen = 0;
    const unsigned char* asc = m_pAacEnc->GetAudioSpecificConfig(&ascLen);
    if (!asc || ascLen == 0) {
        // ASC 在 AacEnc 构造时即可生成，正常不会走到这里
        return nullptr;
    }

    C_Fmp4Muxer::VideoConfig vcfg;
    vcfg.width        = (int)m_Wight;
    vcfg.height       = (int)m_Hight;
    vcfg.spsPpsAnnexB = spsPps;
    vcfg.spsPpsLen    = spsPpsLen;

    C_Fmp4Muxer::AudioConfig acfg;
    acfg.sampleRate = (int)m_pAacEnc->SampleRate();
    acfg.channels   = (int)m_pAacEnc->Channels();
    acfg.asc        = asc;
    acfg.ascLen     = ascLen;

    auto muxer = std::make_shared<C_Fmp4Muxer>(this, vcfg, acfg);
    if (!muxer->IsReady()) {
        CLOG_ERR("fMP4 muxer 初始化失败，等待下一帧重试\n");
        return nullptr;
    }
    m_pMuxer = muxer;
    CLOG_INF("fMP4 muxer 初始化完成 (sps/pps=%u, asc=%u)\n", spsPpsLen, ascLen);
    return muxer;
}

//编码器回调的H264数据（Annex-B，可能含 SPS/PPS/IDR/P）
int C_Terminal::OnOutputH264(unsigned char* data, unsigned int dataLen,
                             int64_t ptsUs, bool isKey)
{
    //此处可控制h264文件写入文件，用于临时测试数据是否正常
    if(false){
        auto fileDeleter = [](std::ofstream* pobj){ pobj->close(); delete pobj; };
        static auto outputFile = std::unique_ptr<std::ofstream, decltype(fileDeleter)>(
            new std::ofstream("encode.h264", std::ios::out | std::ios::binary),
            fileDeleter
        );
        if(outputFile->is_open()){
            outputFile->write((const char*)data, dataLen);
        }
    }

    auto muxer = GetOrCreateMuxer();
    if (muxer) muxer->InputH264(data, dataLen, ptsUs, isKey);
    return 0;
}

//音频编码回调的aac数据
int C_Terminal::OnOutputAac(unsigned char* data, unsigned int dataLen, int64_t ptsUs)
{
    // ---- 纯音频直播路径（/ws/audio）----
    // 不依赖 muxer / 视频是否就绪，只要 AAC 出帧就把 ADTS 帧广播出去。
    // 这样浏览器端"仅音频模式"在没有 H264 IDR 之前也能立即出声。
    if (m_pWsServer) {
        const int sr = (int)m_pAacEnc->SampleRate();
        const int ch = (int)m_pAacEnc->Channels();
        std::vector<unsigned char> adts;
        BuildAdtsFrame(data, dataLen, sr, ch, adts);
        m_pWsServer->BroadcastAudioBinary(adts.data(), adts.size());
    }

    // 复制 shared_ptr 后在锁外输入；即使未来支持运行时重建，本次调用期间对象
    // 生命周期也稳定。muxer 还没建好时丢弃音频，避免先产生无视频 fragment。
    std::shared_ptr<C_Fmp4Muxer> muxer;
    {
        std::lock_guard<std::mutex> lk(m_muxerMutex);
        muxer = m_pMuxer;
    }
    if (muxer) muxer->InputAac(data, dataLen, ptsUs);
    return 0;
}

/*WebSocket新客户端连接事件*/
int C_Terminal::OnNewWSClientConnect(int fd)
{
    CLOG_INF("OnNewWSClientConnect fd:%d\n", fd);
    // 新客户端拿到 init segment 后，要尽快出关键帧才能跑起来；这里强制 I 帧
    m_pH264Enc->ForceIframe();
    return 0;
}

/*WebSocket客户端断开连接事件*/
int C_Terminal::OnWSClientDisconnect(int fd)
{
    CLOG_INF("OnWSClientDisconnect fd:%d\n", fd);
    // 若是 /ws/talk 客户端断开：清掉 fd 标记 + 重置解码器/PCM，避免拼到下一次会话；
    // 当所有讲话客户端都走光（1→0）时 Shutdown 整个 TalkPlayer，让出独占的 /dev/snd
    bool wasTalk = false;
    bool wasLast = false;
    {
        std::lock_guard<std::mutex> lk(m_talkFdsMutex);
        wasTalk = m_talkFds.erase(fd) > 0;
        wasLast = wasTalk && m_talkFds.empty();
    }
    if (wasTalk && m_pTalkPlayer) {
        m_pTalkPlayer->Reset();
        if (wasLast) {
            // 最后一个讲话客户端断开：彻底释放 ALSA + FFmpeg 解码器，让别的播放路径
            //（TTS / 报警声等将来用法）能拿到声卡。下次有人再连讲话会重新 InitDevice。
            CLOG_INF("最后一个讲话客户端已断开，关闭 TalkPlayer 释放声卡\n");
            m_pTalkPlayer->Shutdown();
        }
    }

    // /ws/log 订阅者断开：更新计数（归 0 后广播器回到零开销空转）。
    bool wasLog = false;
    size_t logCnt = 0;
    {
        std::lock_guard<std::mutex> lk(m_logFdsMutex);
        wasLog = m_logFds.erase(fd) > 0;
        logCnt = m_logFds.size();
    }
    if (wasLog && m_pLogBroadcaster) m_pLogBroadcaster->SetClientCount((int)logCnt);
    return 0;
}

/*接收到WebSocket客户端消息*/
int C_Terminal::OnWSClientMessage(int fd, const std::vector<unsigned char>& data)
{
    // 1) /ws/talk 客户端：所有 binary 帧都是 OPUS，直接喂解码器
    bool isTalk = false;
    {
        std::lock_guard<std::mutex> lk(m_talkFdsMutex);
        isTalk = m_talkFds.count(fd) > 0;
    }
    if (isTalk) {
        if (m_pTalkPlayer && !data.empty()) {
            m_pTalkPlayer->FeedOpus(data.data(), (unsigned int)data.size());
        }
        return 0;
    }

    // 2) 直播控制通道（兼容老协议）：单字节 0xFF 请求强制 I 帧
    if (data.size() > 0) {
        unsigned char cmd = data[0];
        CLOG_INF("OnWSClientMessage fd:%d cmd:%d\n", fd, cmd);

        if (cmd == 0xFF) {
            CLOG_INF("Received key frame request from fd:%d\n", fd);
            m_pH264Enc->ForceIframe();
            return 0;
        }
    }
    return 0;
}

/*握手完成回调（用于区分 /ws/talk vs /ws/live）
  注意：本回调由 m_pWsServer 与 m_pWsFileServer 共用，因此只能根据 urlPath 判断 */
void C_Terminal::OnWSClientHandshake(int fd, const std::string& urlPath)
{
    if (urlPath == "/ws/talk" || urlPath == "/ws/talk/") {
        bool needInit = false;
        {
            std::lock_guard<std::mutex> lk(m_talkFdsMutex);
            // 0→1 切换时才需要 InitDevice；2、3 个并发讲话客户端共用同一个解码线程
            needInit = m_talkFds.empty();
            m_talkFds.insert(fd);
            CLOG_INF("讲话客户端已连接: fd=%d (count=%zu)\n", fd, m_talkFds.size());
        }
        // InitDevice 在锁外调用，避免 ALSA/avcodec_open2 慢操作把握手回调阻塞太久
        if (needInit && m_pTalkPlayer) {
            CLOG_INF("首个讲话客户端连入，启动 TalkPlayer（打开声卡 + 加载 OPUS 解码器）\n");
            if (!m_pTalkPlayer->InitDevice()) {
                CLOG_ERR("TalkPlayer InitDevice 失败：声卡可能被其它进程占用，本次讲话不可用\n");
            }
        }
    } else if (urlPath == "/ws/log" || urlPath == "/ws/log/") {
        // 日志订阅者接入：更新计数，广播器据此决定是否入队（0 人时零开销）。
        size_t cnt = 0;
        {
            std::lock_guard<std::mutex> lk(m_logFdsMutex);
            m_logFds.insert(fd);
            cnt = m_logFds.size();
        }
        if (m_pLogBroadcaster) m_pLogBroadcaster->SetClientCount((int)cnt);
        CLOG_INF("日志订阅客户端已连接: fd=%d (count=%zu)\n", fd, cnt);
    }
}

/*fmp4Muxer 输出 init segment（ftyp+moov）*/
void C_Terminal::OnInitSegment(const uint8_t* data, size_t len)
{
    CLOG_INF("fMP4 init segment 就绪 (%zu bytes), 推送至 LiveHub\n", len);
    C_LiveHub::Inst().PushInitSegment(data, len);
}

/*fmp4Muxer 输出一个 fragment*/
void C_Terminal::OnFragment(const uint8_t* data, size_t len)
{
    C_LiveHub::Inst().PushFragment(data, len);
}

// RGB888 转 NV21
void C_Terminal::rgb888ToNv21(const unsigned char* rgb, unsigned char* nv21, int width, int height) {
    int yIndex = 0;
    int uvIndex = width * height;
    int index = 0;

    for (int j = 0; j < height; ++j) {
        for (int i = 0; i < width; ++i) {
            int r = rgb[index++];
            int g = rgb[index++];
            int b = rgb[index++];

            // 计算Y分量
            nv21[yIndex++] = static_cast<unsigned char>((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;

            // 计算U和V分量，每2x2的块共享一个U和一个V
            if (j % 2 == 0 && i % 2 == 0) {
                // 计算V和U分量（注意顺序，V在前，U在后）
                int v = static_cast<int>((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
                int u = static_cast<int>((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;

                // 存储V和U分量
                nv21[uvIndex++] = static_cast<unsigned char>(v);
                nv21[uvIndex++] = static_cast<unsigned char>(u);
            }
        }
    }
}

// 获取 IPv4 地址的接口
std::string C_Terminal::get_ipv4_address() {
    struct ifaddrs *ifaddr, *ifa;
    char host[NI_MAXHOST];
    std::string ipv4_address = "0.0.0.0";

    // 获取网络接口信息
    if (getifaddrs(&ifaddr) == -1) {
        perror("getifaddrs");
        return "";
    }

    // 遍历所有网络接口
    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL)
            continue;

        int family = ifa->ifa_addr->sa_family;

        // 只处理 IPv4 地址
        if (family == AF_INET) {
            int s = getnameinfo(ifa->ifa_addr, sizeof(struct sockaddr_in), host, NI_MAXHOST, NULL, 0, NI_NUMERICHOST);
            if (s != 0) {
                std::cerr << "getnameinfo() failed: " << gai_strerror(s) << std::endl;
                continue;
            }
            // 找到第一个 IPv4 地址并返回
            ipv4_address = host;
            if(ipv4_address == "127.0.0.1"){ continue; } //找到的为127.0.0.1本地回环地址跳过
            break;
        }
    }

    freeifaddrs(ifaddr); // 释放资源
    return ipv4_address;
}

// ----------------------------------------------------------------------
// HTTP API 注册：监控录像（常驻、按天分目录）
//
// 全部走 80 端口；前端通过 fetch 调用，与 8081 (live ws) 互不影响。
//
//   GET  /api/record/status      -> {recording, file, bytes, root}
//   GET  /api/record/days        -> ["20260523", "20260522", ...]   (倒序)
//   GET  /api/record/segments?date=YYYYMMDD
//                                -> [{name, size, hms, sec}, ...]   (倒序)
//   GET  /record/<YYYYMMDD>/<name>.mp4
//                                -> 录像分片下载/inline 播放
//
// 不再暴露 start/stop —— 监控录像默认常驻。前端只读状态、列出日期、列出分片。
// ----------------------------------------------------------------------
namespace {
// 简易 JSON 转义（仅处理 \\、\"、\n）
std::string JsonEscape(const std::string& s)
{
    std::string r;
    r.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '\\': r += "\\\\"; break;
            case '"':  r += "\\\""; break;
            case '\n': r += "\\n";  break;
            case '\r': r += "\\r";  break;
            case '\t': r += "\\t";  break;
            default:   r += c;
        }
    }
    return r;
}

// 取文件名 basename
std::string BaseName(const std::string& path)
{
    size_t s = path.find_last_of('/');
    return (s == std::string::npos) ? path : path.substr(s + 1);
}

// 注：原本这里有一个 ReadFileAll，把整文件读进 std::string 后塞 ApiResponse::body。
// 录像/相册接口已经改走 httpServer 的 ApiResponse::filePath 流式分支，
// 因此这里也不再需要这个工具函数（避免有人误用又引入 OOM 风险）。

// 校验 YYYYMMDD（仅 8 位数字）
bool IsDayStr(const std::string& s) {
    if (s.size() != 8) return false;
    for (char c : s) if (c < '0' || c > '9') return false;
    return true;
}

// 仅允许 [A-Za-z0-9_.-]
bool IsSafeFileName(const std::string& s) {
    if (s.empty() || s.find("..") != std::string::npos) return false;
    for (char c : s) {
        bool ok = (c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||
                  c=='_'||c=='.'||c=='-';
        if (!ok) return false;
    }
    return true;
}

// 从极简 JSON body 里抽 "key":"value"（仅字符串值，支持 \" \\ 转义）。
// 找不到返回 false。与 /api/prompt、/api/photo/delete 的手写解析同风格，
// 只覆盖本项目自己前端发出的扁平对象，不追求通用 JSON 兼容。
bool ExtractJsonStr(const std::string& body, const std::string& key,
                    std::string& out) {
    const std::string pat = "\"" + key + "\"";
    size_t kp = body.find(pat);
    if (kp == std::string::npos) return false;
    size_t colon = body.find(':', kp + pat.size());
    if (colon == std::string::npos) return false;
    size_t i = colon + 1;
    while (i < body.size() &&
           (body[i]==' '||body[i]=='\t'||body[i]=='\n'||body[i]=='\r')) ++i;
    if (i >= body.size() || body[i] != '"') return false;
    ++i;
    std::string v;
    while (i < body.size()) {
        char c = body[i++];
        if (c == '"') { out = v; return true; }
        if (c == '\\' && i < body.size()) {
            char e = body[i++];
            switch (e) {
                case 'n': v += '\n'; break;
                case 'r': v += '\r'; break;
                case 't': v += '\t'; break;
                case '"': v += '"';  break;
                case '\\':v += '\\'; break;
                case '/': v += '/';  break;
                default:  v += e;    break;
            }
        } else {
            v += c;
        }
    }
    return false;  // 未闭合
}
} // namespace

void C_Terminal::RegisterHttpApis()
{
    auto* http = m_pHttpServer.get();

    http->RegisterApiPrefix("GET", "/.well-known/acme-challenge/",
        [this](const C_HttpServer::ApiRequest& req) -> C_HttpServer::ApiResponse {
            C_HttpServer::ApiResponse rsp;
            const std::string prefix = "/.well-known/acme-challenge/";
            const std::string token = req.path.substr(prefix.size());
            if (token.empty() || token.size() > 256 ||
                !std::all_of(token.begin(), token.end(), [](unsigned char c) {
                    return std::isalnum(c) || c == '-' || c == '_';
                })) {
                rsp.status = 400;
                rsp.contentType = "text/plain; charset=utf-8";
                rsp.body = "invalid challenge token";
                return rsp;
            }
            const std::string path = m_acmeChallengeDir + "/" + token;
            struct stat st{};
            if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
                rsp.status = 404;
                rsp.contentType = "text/plain; charset=utf-8";
                rsp.body = "challenge not found";
                return rsp;
            }
            rsp.contentType = "text/plain; charset=utf-8";
            rsp.filePath = path;
            return rsp;
        });

    // GET /api/record/status  ->  当前正在录的文件 / 字节数 / 根目录
    http->RegisterApi("GET", "/api/record/status",
        [this](const C_HttpServer::ApiRequest&) -> C_HttpServer::ApiResponse {
            C_HttpServer::ApiResponse rsp;
            std::ostringstream js;
            const auto config = C_AppConfig::GetInst().GetSnapshot();
            js << "{\"enabled\":" << (config.record_enabled ? "true" : "false")
               << ",\"recording\":" << (m_pRecorder->IsRecording() ? "true" : "false")
               << ",\"file\":\"" << JsonEscape(BaseName(m_pRecorder->CurrentFile())) << "\""
               << ",\"bytes\":" << m_pRecorder->CurrentBytes()
               << ",\"root\":\"" << JsonEscape(m_pRecorder->RootDir()) << "\""
               << "}";
            rsp.body = js.str();
            return rsp;
        });

    // GET /api/record/days  ->  ["20260523", "20260522", ...]，倒序
    http->RegisterApi("GET", "/api/record/days",
        [this](const C_HttpServer::ApiRequest&) -> C_HttpServer::ApiResponse {
            C_HttpServer::ApiResponse rsp;
            std::vector<std::string> days;
            DIR* d = opendir(m_recordDir.c_str());
            if (d) {
                struct dirent* e = nullptr;
                while ((e = readdir(d)) != nullptr) {
                    std::string n(e->d_name);
                    if (!IsDayStr(n)) continue;
                    std::string full = m_recordDir + "/" + n;
                    struct stat st{};
                    if (stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
                        days.push_back(n);
                    }
                }
                closedir(d);
            }
            std::sort(days.begin(), days.end(), std::greater<std::string>());
            std::ostringstream js;
            js << "[";
            for (size_t i = 0; i < days.size(); ++i) {
                if (i) js << ",";
                js << "\"" << days[i] << "\"";
            }
            js << "]";
            rsp.body = js.str();
            return rsp;
        });

    // GET /api/record/segments?date=YYYYMMDD
    //   -> [{name, size, hms, sec}, ...]，按时间升序，方便前端时间轴绘制
    //      hms = "HH:MM:SS"；sec = 当天 0 点起的秒数（用于时间轴定位）
    //      不传 date 则用今天
    http->RegisterApi("GET", "/api/record/segments",
        [this](const C_HttpServer::ApiRequest& req) -> C_HttpServer::ApiResponse {
            C_HttpServer::ApiResponse rsp;

            // 从 query 解析 date=YYYYMMDD（不传则用今天）
            std::string date;
            {
                const std::string& q = req.query;
                size_t pos = q.find("date=");
                if (pos != std::string::npos) {
                    pos += 5;
                    size_t end = q.find('&', pos);
                    date = q.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
                }
            }
            if (date.empty()) {
                // 与 recorder 命名保持一致：UTC + 8h
                static constexpr int kChinaTimeOffsetSec = 8 * 60 * 60;
                std::time_t t = std::time(nullptr) + kChinaTimeOffsetSec;
                std::tm tmv{}; localtime_r(&t, &tmv);
                char buf[16] = {0};
                std::strftime(buf, sizeof(buf), "%Y%m%d", &tmv);
                date = buf;
            }
            if (!IsDayStr(date)) {
                rsp.status = 400; rsp.contentType = "text/plain";
                rsp.body = "bad date"; return rsp;
            }

            std::string dayDir = m_recordDir + "/" + date;
            std::vector<std::tuple<std::string,uint64_t,int>> segs;  // name, size, secOfDay
            DIR* d = opendir(dayDir.c_str());
            if (d) {
                struct dirent* e = nullptr;
                while ((e = readdir(d)) != nullptr) {
                    std::string n(e->d_name);
                    if (n.size() < 5) continue;
                    if (n.compare(n.size() - 4, 4, ".mp4") != 0) continue;
                    // 文件名格式：YYYYMMDD_HHMMSS.mp4，第 9..14 是 HHMMSS
                    if (n.size() < 19) continue;
                    int hh = std::atoi(n.substr(9,  2).c_str());
                    int mm = std::atoi(n.substr(11, 2).c_str());
                    int ss = std::atoi(n.substr(13, 2).c_str());
                    int sec = hh * 3600 + mm * 60 + ss;
                    std::string full = dayDir + "/" + n;
                    struct stat st{};
                    if (stat(full.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
                        segs.emplace_back(n, (uint64_t)st.st_size, sec);
                    }
                }
                closedir(d);
            }
            std::sort(segs.begin(), segs.end(),
                      [](const auto& a, const auto& b){
                          return std::get<0>(a) < std::get<0>(b);
                      });

            std::ostringstream js;
            js << "[";
            for (size_t i = 0; i < segs.size(); ++i) {
                if (i) js << ",";
                const auto& s = segs[i];
                int sec = std::get<2>(s);
                int hh = sec / 3600, mm = (sec / 60) % 60, ss = sec % 60;
                char hms[16] = {0};
                std::snprintf(hms, sizeof(hms), "%02d:%02d:%02d", hh, mm, ss);
                js << "{\"name\":\"" << JsonEscape(std::get<0>(s))
                   << "\",\"size\":" << std::get<1>(s)
                   << ",\"hms\":\""  << hms << "\""
                   << ",\"sec\":"    << sec
                   << "}";
            }
            js << "]";
            rsp.body = js.str();
            return rsp;
        });

    // GET /record/<YYYYMMDD>/<name>.mp4  ->  录像文件下载 / inline 播放
    //
    // 关键：返回 filePath 走 httpServer 流式分支，不再 ReadFileAll。
    // 早期版本把整个 mp4 (~600KB-几 MB) 读进 ApiResponse::body，浏览器
    // 在回放页面来回点击就会触发并发请求 → 多份 std::string 拷贝 → 内核
    // OOM-killer 把 camera 进程干掉。改走 filePath 之后单次请求常驻只有
    // 64KB 缓冲，且 httpServer 自动响应 Range 头返回 206，配合浏览器拖动
    // 不再每次都全量下载。
    http->RegisterApiPrefix("GET", "/record/",
        [this](const C_HttpServer::ApiRequest& req) -> C_HttpServer::ApiResponse {
            C_HttpServer::ApiResponse rsp;
            // 截掉前缀 "/record/"
            std::string sub = req.path.substr(strlen("/record/"));
            // 期望格式： YYYYMMDD/xxx.mp4
            size_t slash = sub.find('/');
            if (slash == std::string::npos) {
                rsp.status = 400; rsp.contentType = "text/plain";
                rsp.body = "bad path"; return rsp;
            }
            std::string day = sub.substr(0, slash);
            std::string name = sub.substr(slash + 1);
            // name 形如 "20260524_001207.mp4" 或 "20260524_001207.mp4.idx"
            //   .idx 是给精准跳转用的伴生索引文件（同名 + ".idx" 后缀）
            // IsSafeFileName 只禁 "..", 多个 "." 是允许的，所以 mp4.idx 直接通过
            bool isIdx = (name.size() > 4 &&
                          name.compare(name.size()-4, 4, ".idx") == 0);
            if (!IsDayStr(day) || !IsSafeFileName(name)) {
                rsp.status = 400; rsp.contentType = "text/plain";
                rsp.body = "bad path"; return rsp;
            }
            std::string full = m_recordDir + "/" + day + "/" + name;
            struct stat st{};
            if (stat(full.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
                rsp.status = 404; rsp.contentType = "text/plain";
                rsp.body = "not found"; return rsp;
            }
            rsp.contentType = isIdx ? "application/json" : "video/mp4";
            rsp.filePath    = std::move(full);  // 流式 + Range
            return rsp;
        });

    // ====================================================================
    // 拍照 API（独立目录 snapshot/，仅手动触发，不自动清理）
    //
    //   POST /api/snapshot                 -> {ok, name, size}
    //   GET  /api/photo/latest             -> {ok, name, size, mtime}
    //   GET  /api/photo/list               -> [{name, size, mtime}, ...]  (倒序)
    //   POST /api/photo/delete   body: {"names":["xxx.jpg",...]}
    //                                      -> {ok, deleted, missing, errors:[...]}
    //   GET  /photo/<name>.jpg             -> 图片字节
    // ====================================================================

    // POST /api/snapshot
    http->RegisterApi("POST", "/api/snapshot",
        [this](const C_HttpServer::ApiRequest&) -> C_HttpServer::ApiResponse {
            C_HttpServer::ApiResponse rsp;
            std::string full;
            std::string name = m_pSnapshot ? m_pSnapshot->TakeOne(&full) : "";
            if (name.empty()) {
                rsp.status = 503; rsp.contentType = "application/json";
                rsp.body = "{\"ok\":false,\"err\":\"snapshot failed (no frame yet?)\"}";
                return rsp;
            }
            struct stat st{};
            uint64_t sz = (stat(full.c_str(), &st) == 0) ? (uint64_t)st.st_size : 0;
            std::ostringstream js;
            js << "{\"ok\":true,\"name\":\"" << JsonEscape(name)
               << "\",\"size\":" << sz << "}";
            rsp.body = js.str();
            return rsp;
        });

    // POST /api/prompt   body: {"name":"door"}
    // 同步播放 <exeDir>/prompt/<name>.wav，要求 16-bit / 48kHz / mono PCM。
    // 返回：
    //   200 {"ok":true, "name":"door", "duration_ms":1410}
    //   400 {"ok":false,"err":"missing name"} / {"ok":false,"err":"bad name"}
    //   404 {"ok":false,"err":"not found"}
    //   409 {"ok":false,"err":"busy"}                     另一个提示音正在播
    //   500 {"ok":false,"err":"play failed"}              ALSA 失败 / wav 格式不符
    http->RegisterApi("POST", "/api/prompt",
        [this](const C_HttpServer::ApiRequest& req) -> C_HttpServer::ApiResponse {
            C_HttpServer::ApiResponse rsp;
            rsp.contentType = "application/json";

            // 极简 json：抽 "name":"xxx"。和 /api/photo/delete 的写法保持一致。
            std::string name;
            const std::string& body = req.body;
            size_t kp = body.find("\"name\"");
            if (kp != std::string::npos) {
                size_t colon = body.find(':', kp);
                if (colon != std::string::npos) {
                    size_t q1 = body.find('"', colon + 1);
                    size_t q2 = (q1 == std::string::npos)
                                  ? std::string::npos : body.find('"', q1 + 1);
                    if (q1 != std::string::npos && q2 != std::string::npos) {
                        name = body.substr(q1 + 1, q2 - q1 - 1);
                    }
                }
            }
            if (name.empty()) {
                rsp.status = 400;
                rsp.body = "{\"ok\":false,\"err\":\"missing name\"}";
                return rsp;
            }
            // 白名单：和 IsSafeFileName 等价，但禁止 '.' 出现在 name 自身（避免传入 "../foo"
            // 或 "foo.wav"），由后端固定拼 .wav 扩展名。
            for (char c : name) {
                bool ok = (c>='A'&&c<='Z')||(c>='a'&&c<='z')||
                          (c>='0'&&c<='9')||c=='_'||c=='-';
                if (!ok) {
                    rsp.status = 400;
                    rsp.body = "{\"ok\":false,\"err\":\"bad name\"}";
                    return rsp;
                }
            }

            std::string full = m_promptDir + "/" + name + ".wav";
            struct stat st{};
            if (stat(full.c_str(), &st) != 0) {
                rsp.status = 404;
                rsp.body = "{\"ok\":false,\"err\":\"not found\"}";
                return rsp;
            }
            // door.wav 标准 wav 头：data 子块 = file_size - dataOff；
            // 这里粗略按 (size - 44) / 2 / 48 估时长（毫秒），仅用于前端 UI 反馈，
            // 误差几十毫秒不影响体验。
            uint64_t durMs = (st.st_size > 44)
                ? (uint64_t)((st.st_size - 44) / 2 / 48)
                : 0;

            if (!m_pTalkPlayer) {
                rsp.status = 500;
                rsp.body = "{\"ok\":false,\"err\":\"player not ready\"}";
                return rsp;
            }
            int rc = m_pTalkPlayer->PlayWavSync(full);
            if (rc == -3) {
                rsp.status = 409;
                rsp.body = "{\"ok\":false,\"err\":\"busy\"}";
                return rsp;
            }
            if (rc != 0) {
                rsp.status = 500;
                rsp.body = "{\"ok\":false,\"err\":\"play failed\"}";
                return rsp;
            }
            std::ostringstream js;
            js << "{\"ok\":true,\"name\":\"" << JsonEscape(name)
               << "\",\"duration_ms\":" << durMs << "}";
            rsp.body = js.str();
            return rsp;
        });

    // GET /api/photo/latest
    http->RegisterApi("GET", "/api/photo/latest",
        [this](const C_HttpServer::ApiRequest&) -> C_HttpServer::ApiResponse {
            C_HttpServer::ApiResponse rsp;
            std::string bestName;
            uint64_t bestSize = 0;
            int64_t bestMtime = 0;

            DIR* d = opendir(m_snapshotDir.c_str());
            if (d) {
                struct dirent* e = nullptr;
                while ((e = readdir(d)) != nullptr) {
                    std::string n(e->d_name);
                    if (n.size() < 5) continue;
                    if (n.compare(n.size() - 4, 4, ".jpg") != 0) continue;
                    if (!IsSafeFileName(n)) continue;
                    if (!bestName.empty() && n <= bestName) continue;

                    std::string full = m_snapshotDir + "/" + n;
                    struct stat st{};
                    if (stat(full.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
                        bestName = n;
                        bestSize = (uint64_t)st.st_size;
                        bestMtime = (int64_t)st.st_mtime;
                    }
                }
                closedir(d);
            }

            rsp.contentType = "application/json";
            if (bestName.empty()) {
                rsp.status = 404;
                rsp.body = "{\"ok\":false,\"err\":\"no photo\"}";
                return rsp;
            }

            std::ostringstream js;
            js << "{\"ok\":true,\"name\":\"" << JsonEscape(bestName)
               << "\",\"size\":" << bestSize
               << ",\"mtime\":" << bestMtime
               << "}";
            rsp.body = js.str();
            return rsp;
        });

    // GET /api/photo/list
    http->RegisterApi("GET", "/api/photo/list",
        [this](const C_HttpServer::ApiRequest&) -> C_HttpServer::ApiResponse {
            C_HttpServer::ApiResponse rsp;
            std::vector<std::tuple<std::string,uint64_t,int64_t>> items;
            DIR* d = opendir(m_snapshotDir.c_str());
            if (d) {
                struct dirent* e = nullptr;
                while ((e = readdir(d)) != nullptr) {
                    std::string n(e->d_name);
                    if (n.size() < 5) continue;
                    if (n.compare(n.size() - 4, 4, ".jpg") != 0) continue;
                    if (!IsSafeFileName(n)) continue;
                    std::string full = m_snapshotDir + "/" + n;
                    struct stat st{};
                    if (stat(full.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
                        items.emplace_back(n, (uint64_t)st.st_size, (int64_t)st.st_mtime);
                    }
                }
                closedir(d);
            }
            // 名字本身就是时间序，按 name 倒序 = 最新在前
            std::sort(items.begin(), items.end(),
                      [](const auto& a, const auto& b){
                          return std::get<0>(a) > std::get<0>(b);
                      });
            std::ostringstream js;
            js << "[";
            for (size_t i = 0; i < items.size(); ++i) {
                if (i) js << ",";
                js << "{\"name\":\""  << JsonEscape(std::get<0>(items[i]))
                   << "\",\"size\":"  << std::get<1>(items[i])
                   << ",\"mtime\":"   << std::get<2>(items[i])
                   << "}";
            }
            js << "]";
            rsp.body = js.str();
            return rsp;
        });

    // POST /api/photo/delete   body: {"names":["aa.jpg","bb.jpg",...]}
    //   - 极简 JSON 解析：手抓 "names":[...] 数组里的每个 "..." 字符串
    //   - 同时支持单文件简化形式 {"name":"xxx.jpg"}
    http->RegisterApi("POST", "/api/photo/delete",
        [this](const C_HttpServer::ApiRequest& req) -> C_HttpServer::ApiResponse {
            C_HttpServer::ApiResponse rsp;
            const std::string& body = req.body;
            std::vector<std::string> names;

            auto extractStr = [](const std::string& s, size_t pos) -> std::pair<std::string,size_t> {
                // 从 pos 起跳过空白，找到 " ... "（不处理转义，文件名只允许安全字符）
                while (pos < s.size() && (s[pos]==' '||s[pos]=='\t'||s[pos]=='\n'||s[pos]=='\r')) ++pos;
                if (pos >= s.size() || s[pos] != '"') return {"", std::string::npos};
                size_t q1 = pos + 1;
                size_t q2 = s.find('"', q1);
                if (q2 == std::string::npos) return {"", std::string::npos};
                return { s.substr(q1, q2 - q1), q2 + 1 };
            };

            size_t arrPos = body.find("\"names\"");
            if (arrPos != std::string::npos) {
                size_t lb = body.find('[', arrPos);
                size_t rb = (lb == std::string::npos) ? std::string::npos : body.find(']', lb);
                if (lb != std::string::npos && rb != std::string::npos) {
                    size_t p = lb + 1;
                    while (p < rb) {
                        auto pr = extractStr(body, p);
                        if (pr.second == std::string::npos) break;
                        if (!pr.first.empty()) names.push_back(pr.first);
                        p = pr.second;
                        // 跳过逗号
                        while (p < rb && (body[p]==' '||body[p]==','||body[p]=='\t'||body[p]=='\n')) ++p;
                    }
                }
            } else {
                size_t nmPos = body.find("\"name\"");
                if (nmPos != std::string::npos) {
                    size_t colon = body.find(':', nmPos);
                    if (colon != std::string::npos) {
                        auto pr = extractStr(body, colon + 1);
                        if (!pr.first.empty()) names.push_back(pr.first);
                    }
                }
            }

            int deleted = 0, missing = 0;
            std::vector<std::string> errors;
            for (const auto& n : names) {
                if (!IsSafeFileName(n) || n.size() < 5 ||
                    n.compare(n.size()-4, 4, ".jpg") != 0) {
                    errors.push_back(n + ":bad-name");
                    continue;
                }
                std::string full = m_snapshotDir + "/" + n;
                struct stat st{};
                if (stat(full.c_str(), &st) != 0) { missing++; continue; }
                if (unlink(full.c_str()) == 0) deleted++;
                else errors.push_back(n + ":unlink-fail");
            }
            std::ostringstream js;
            js << "{\"ok\":true,\"deleted\":" << deleted
               << ",\"missing\":" << missing
               << ",\"errors\":[";
            for (size_t i = 0; i < errors.size(); ++i) {
                if (i) js << ",";
                js << "\"" << JsonEscape(errors[i]) << "\"";
            }
            js << "]}";
            rsp.body = js.str();
            return rsp;
        });

    // GET /photo/<name>.jpg —— 同样走流式 + Range，避免大图把内存吃满
    http->RegisterApiPrefix("GET", "/photo/",
        [this](const C_HttpServer::ApiRequest& req) -> C_HttpServer::ApiResponse {
            C_HttpServer::ApiResponse rsp;
            std::string name = req.path.substr(strlen("/photo/"));
            if (!IsSafeFileName(name)) {
                rsp.status = 400; rsp.contentType = "text/plain";
                rsp.body = "bad path"; return rsp;
            }
            std::string full = m_snapshotDir + "/" + name;
            struct stat st{};
            if (stat(full.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
                rsp.status = 404; rsp.contentType = "text/plain";
                rsp.body = "not found"; return rsp;
            }
            rsp.contentType = "image/jpeg";
            rsp.filePath    = std::move(full);  // 流式 + Range
            return rsp;
        });

    // ====================================================================
    // 配置 API（运行时配置中心 C_AppConfig）
    //
    //   GET  /api/config           -> 返回完整 snapshot JSON
    //   POST /api/config           body: {"key":"<value>", ...}
    //                              -> 应用 patch 并返回更新后的 snapshot；
    //                                 仅识别已知 key，未知 key 静默忽略；
    //                                 非法值（越界 / 类型错）会被 clamp 到合法区间
    // ====================================================================

    http->RegisterApi("GET", "/api/config",
        [](const C_HttpServer::ApiRequest&) -> C_HttpServer::ApiResponse {
            C_HttpServer::ApiResponse rsp;
            rsp.contentType = "application/json";
            rsp.body = C_AppConfig::ToJson(C_AppConfig::GetInst().GetSnapshot());
            return rsp;
        });

    // GET /api/netinfo  ->  设备真实网卡地址 {"ipv4":"...", "ipv6":"..."}
    // 浏览器地址栏的 host 只是"访问入口"，不一定等于设备当前网卡地址；这里直接
    // 从板端枚举网卡返回，供前端在实况页展示。IPv6 取配置里监测网卡的全局单播地址
    // （复用 MqttReporter 的实现，已排除 fe80:: / ::1），拿不到时返回空串。
    http->RegisterApi("GET", "/api/netinfo",
        [](const C_HttpServer::ApiRequest&) -> C_HttpServer::ApiResponse {
            C_HttpServer::ApiResponse rsp;
            rsp.contentType = "application/json";
            const std::string iface = C_AppConfig::GetInst().GetSnapshot().mqtt_iface;
            std::string ipv6 = C_MqttReporter::GetGlobalIpv6(iface);
            if (ipv6.empty() && !iface.empty()) {
                // 监测网卡上没有全局 IPv6 时，退一步扫描所有网卡兜底
                ipv6 = C_MqttReporter::GetGlobalIpv6("");
            }
            std::ostringstream js;
            js << "{\"ipv4\":\"" << JsonEscape(C_Terminal::get_ipv4_address()) << "\""
               << ",\"ipv6\":\"" << JsonEscape(ipv6) << "\""
               << "}";
            rsp.body = js.str();
            return rsp;
        });

    // GET /api/sysinfo  ->  设备系统资源快照
    //   { cpu:{valid,percent,cores}, load:{valid,l1,l5,l15},
    //     mem:{valid,total_kb,avail_kb}, proc:{valid,vmrss_kb,vmdata_kb,threshold_kb},
    //     disk:{valid,total_bytes,avail_bytes}, uptime:{valid,sec}, proc_uptime:{valid,sec} }
    // 全部读 procfs / statvfs（只读、无副作用）。CPU% 为两次请求间的 /proc/stat 差值，
    // 首次请求 cpu.valid=false（无历史样本）。proc.threshold_kb 回显 mem_watchdog 阈值。
    http->RegisterApi("GET", "/api/sysinfo",
        [this](const C_HttpServer::ApiRequest&) -> C_HttpServer::ApiResponse {
            C_HttpServer::ApiResponse rsp;
            rsp.contentType = "application/json";
            if (!m_pSysInfo) {
                rsp.status = 503;
                rsp.body = "{\"ok\":false,\"err\":\"sysinfo unavailable\"}";
                return rsp;
            }
            // mem_watchdog.sh 的 VmData 阈值：40MB = 40960KB。这里回显，供 web 显示
            // "进程内存 / 重启阈值"，与看门狗保持一致。
            const uint64_t kWatchdogThresholdKb = 40960;
            // statvfs 用录像目录：它落在 eMMC 用户分区，正是关心"还能录多久"的那块。
            C_SysInfoProvider::Info in = m_pSysInfo->Sample(m_recordDir, kWatchdogThresholdKb);

            std::ostringstream js;
            js << "{\"ok\":true"
               << ",\"cpu\":{\"valid\":"  << (in.cpuValid ? "true":"false")
               <<   ",\"percent\":"       << (in.cpuValid ? in.cpuPercent : 0.0)
               <<   ",\"cores\":"         << in.cpuCores << "}"
               << ",\"load\":{\"valid\":" << (in.loadValid ? "true":"false")
               <<   ",\"l1\":"  << in.load1 << ",\"l5\":" << in.load5 << ",\"l15\":" << in.load15 << "}"
               << ",\"mem\":{\"valid\":"  << (in.memValid ? "true":"false")
               <<   ",\"total_kb\":"      << (unsigned long long)in.memTotalKb
               <<   ",\"avail_kb\":"      << (unsigned long long)in.memAvailKb << "}"
               << ",\"proc\":{\"valid\":" << (in.procValid ? "true":"false")
               <<   ",\"vmrss_kb\":"      << (unsigned long long)in.procVmRssKb
               <<   ",\"vmdata_kb\":"     << (unsigned long long)in.procVmDataKb
               <<   ",\"threshold_kb\":"  << (unsigned long long)in.watchdogThresholdKb << "}"
               << ",\"disk\":{\"valid\":" << (in.diskValid ? "true":"false")
               <<   ",\"total_bytes\":"   << (unsigned long long)in.diskTotalBytes
               <<   ",\"avail_bytes\":"   << (unsigned long long)in.diskAvailBytes << "}"
               << ",\"uptime\":{\"valid\":" << (in.uptimeValid ? "true":"false")
               <<   ",\"sec\":"           << (unsigned long long)in.uptimeSec << "}"
               << ",\"proc_uptime\":{\"valid\":" << (in.procUptimeValid ? "true":"false")
               <<   ",\"sec\":"           << (unsigned long long)in.procUptimeSec << "}"
               << "}";
            rsp.body = js.str();
            return rsp;
        });

    http->RegisterApi("POST", "/api/config",
        [](const C_HttpServer::ApiRequest& req) -> C_HttpServer::ApiResponse {
            C_HttpServer::ApiResponse rsp;
            rsp.contentType = "application/json";

            // 极简 JSON 解析：只识别 {"k1":v1,"k2":v2,...}，
            // value 支持 数字 / true / false / "string"。和 AppConfig 内部 parser 同形式。
            const std::string& body = req.body;
            std::vector<std::pair<std::string,std::string>> kv;

            auto skipWs = [&](size_t& p) {
                while (p < body.size() &&
                       (body[p]==' '||body[p]=='\t'||body[p]=='\n'||body[p]=='\r'))
                    ++p;
            };
            auto readStr = [&](size_t& p, std::string& out) -> bool {
                skipWs(p);
                if (p >= body.size() || body[p] != '"') return false;
                size_t q1 = p + 1;
                size_t q2 = body.find('"', q1);
                if (q2 == std::string::npos) return false;
                out = body.substr(q1, q2 - q1);
                p = q2 + 1;
                return true;
            };
            auto readVal = [&](size_t& p, std::string& out) -> bool {
                skipWs(p);
                if (p >= body.size()) return false;
                if (body[p] == '"') {
                    return readStr(p, out);
                }
                // 数字 / true / false / null
                size_t s = p;
                while (p < body.size() &&
                       body[p] != ',' && body[p] != '}' &&
                       body[p] != ' ' && body[p] != '\t' &&
                       body[p] != '\n' && body[p] != '\r') ++p;
                out = body.substr(s, p - s);
                return !out.empty();
            };

            size_t p = body.find('{');
            if (p == std::string::npos) {
                rsp.status = 400;
                rsp.body = "{\"ok\":false,\"err\":\"bad json\"}";
                return rsp;
            }
            ++p;
            while (true) {
                skipWs(p);
                if (p >= body.size()) break;
                if (body[p] == '}') break;
                std::string k;
                if (!readStr(p, k)) break;
                skipWs(p);
                if (p >= body.size() || body[p] != ':') break;
                ++p;
                std::string v;
                if (!readVal(p, v)) break;
                kv.emplace_back(k, v);
                skipWs(p);
                if (p < body.size() && body[p] == ',') { ++p; continue; }
                break;
            }

            auto newSnap = C_AppConfig::GetInst().ApplyPatch(kv);
            std::ostringstream os;
            os << "{\"ok\":true,\"config\":" << C_AppConfig::ToJson(newSnap) << "}";
            rsp.body = os.str();
            return rsp;
        });

    // ==================================================================
    // 设备动作代理（web 上可增删的"按钮→URL"，点击后由后端出站 POST）
    //
    //   GET  /api/actions              -> {"ok":true,"actions":[{id,name,url},...]}
    //   POST /api/actions   body {"name":"开门","url":"http://..."}
    //                                  -> 201 {"ok":true,"action":{id,name,url}}
    //   POST /api/actions/delete  body {"id":".."}
    //                                  -> {"ok":true} / 404 {"ok":false,"err":"not found"}
    //   POST /api/actions/invoke  body {"id":".."}
    //                                  -> {"ok":true,"status":200} 表示已成功向目标 POST
    //                                     并收到状态行；网络失败返回 502。
    //
    // 仅支持 http:// 目标；配置持久化到 <exeDir>/actions.json。
    // ==================================================================

    // GET /api/actions
    http->RegisterApi("GET", "/api/actions",
        [this](const C_HttpServer::ApiRequest&) -> C_HttpServer::ApiResponse {
            C_HttpServer::ApiResponse rsp;
            rsp.contentType = "application/json";
            std::ostringstream js;
            js << "{\"ok\":true,\"actions\":" << m_pActionStore->ToJson() << "}";
            rsp.body = js.str();
            return rsp;
        });

    // POST /api/actions  ->  新增一个动作
    http->RegisterApi("POST", "/api/actions",
        [this](const C_HttpServer::ApiRequest& req) -> C_HttpServer::ApiResponse {
            C_HttpServer::ApiResponse rsp;
            rsp.contentType = "application/json";

            std::string name, url;
            ExtractJsonStr(req.body, "name", name);
            ExtractJsonStr(req.body, "url",  url);

            std::string id, err;
            if (!m_pActionStore->Add(name, url, id, err)) {
                rsp.status = (err == "too many actions") ? 409 : 400;
                rsp.body = "{\"ok\":false,\"err\":\"" + JsonEscape(err) + "\"}";
                return rsp;
            }
            C_ActionStore::Action a;
            m_pActionStore->Get(id, a);
            rsp.status = 201;
            std::ostringstream js;
            js << "{\"ok\":true,\"action\":{\"id\":\"" << JsonEscape(a.id)
               << "\",\"name\":\"" << JsonEscape(a.name)
               << "\",\"url\":\""  << JsonEscape(a.url) << "\"}}";
            rsp.body = js.str();
            return rsp;
        });

    // POST /api/actions/delete  body {"id":".."}
    http->RegisterApi("POST", "/api/actions/delete",
        [this](const C_HttpServer::ApiRequest& req) -> C_HttpServer::ApiResponse {
            C_HttpServer::ApiResponse rsp;
            rsp.contentType = "application/json";
            std::string id;
            if (!ExtractJsonStr(req.body, "id", id) || id.empty()) {
                rsp.status = 400;
                rsp.body = "{\"ok\":false,\"err\":\"missing id\"}";
                return rsp;
            }
            if (!m_pActionStore->Remove(id)) {
                rsp.status = 404;
                rsp.body = "{\"ok\":false,\"err\":\"not found\"}";
                return rsp;
            }
            rsp.body = "{\"ok\":true}";
            return rsp;
        });

    // POST /api/actions/update  body {"id":"..","name":"..","url":".."}
    // 按 id 就地更新按钮名 / 目标 URL；校验规则与新增一致。
    http->RegisterApi("POST", "/api/actions/update",
        [this](const C_HttpServer::ApiRequest& req) -> C_HttpServer::ApiResponse {
            C_HttpServer::ApiResponse rsp;
            rsp.contentType = "application/json";
            std::string id, name, url;
            if (!ExtractJsonStr(req.body, "id", id) || id.empty()) {
                rsp.status = 400;
                rsp.body = "{\"ok\":false,\"err\":\"missing id\"}";
                return rsp;
            }
            ExtractJsonStr(req.body, "name", name);
            ExtractJsonStr(req.body, "url",  url);

            std::string err;
            if (!m_pActionStore->Update(id, name, url, err)) {
                rsp.status = (err == "not found") ? 404 : 400;
                rsp.body = "{\"ok\":false,\"err\":\"" + JsonEscape(err) + "\"}";
                return rsp;
            }
            C_ActionStore::Action a;
            m_pActionStore->Get(id, a);
            std::ostringstream js;
            js << "{\"ok\":true,\"action\":{\"id\":\"" << JsonEscape(a.id)
               << "\",\"name\":\"" << JsonEscape(a.name)
               << "\",\"url\":\""  << JsonEscape(a.url) << "\"}}";
            rsp.body = js.str();
            return rsp;
        });

    // POST /api/actions/invoke  body {"id":".."}
    // 后端到该动作的 URL 发一次出站 POST（空 body）。这是本功能的核心：
    // 把"浏览器点按钮"代理成"camera 主动请求局域网设备"，规避浏览器跨域/混合内容限制。
    http->RegisterApi("POST", "/api/actions/invoke",
        [this](const C_HttpServer::ApiRequest& req) -> C_HttpServer::ApiResponse {
            C_HttpServer::ApiResponse rsp;
            rsp.contentType = "application/json";
            std::string id;
            if (!ExtractJsonStr(req.body, "id", id) || id.empty()) {
                rsp.status = 400;
                rsp.body = "{\"ok\":false,\"err\":\"missing id\"}";
                return rsp;
            }
            C_ActionStore::Action a;
            if (!m_pActionStore->Get(id, a)) {
                rsp.status = 404;
                rsp.body = "{\"ok\":false,\"err\":\"not found\"}";
                return rsp;
            }
            // 出站 POST 是阻塞的（最多 5s 超时），跑在 HTTP 客户端线程里，
            // 不影响采集/编码/直播线程。
            HttpClient::PostResult pr = HttpClient::Post(a.url);
            if (!pr.ok) {
                rsp.status = 502;   // Bad Gateway：到目标设备的链路失败
                std::ostringstream js;
                js << "{\"ok\":false,\"err\":\"" << JsonEscape(pr.err)
                   << "\",\"name\":\"" << JsonEscape(a.name) << "\"}";
                rsp.body = js.str();
                return rsp;
            }
            std::ostringstream js;
            js << "{\"ok\":true,\"status\":" << pr.status
               << ",\"name\":\"" << JsonEscape(a.name) << "\"}";
            rsp.body = js.str();
            return rsp;
        });
}
