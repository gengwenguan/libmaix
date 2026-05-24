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
#include <sstream>
#include <fstream>
#include <cstring>
#include <tuple>
#include <vector>
#include"logAdapt.h"
#include "terminal.h"
#include "liveHub.h"
#include "appConfig.h"


C_Terminal::C_Terminal(unsigned int Wight, unsigned int Hight, libmaix_cam_t* aiCam)
    :m_Wight(Wight),
    m_Hight(Hight),
    m_pNv12Buff(new unsigned char[Wight*Hight+Wight*Hight/2]),
    // WebSocket 直播：8081（HTTP=8080 顺序），订阅 LiveHub 接收 fMP4
    m_pWsServer(new C_WebSocketServer(this, 8081, true)),
    // WebSocket 回放：8082（保留旧文件回放路径）
    m_pWsFileServer(new C_WebSocketServer(this, 8082, false)),
    m_pHttpServer(new C_HttpServer(8080)),
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
    CLOG_INF("HTTP服务器端口: 8080\n");

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
    mkdir(m_recordDir.c_str(),   0755);
    mkdir(m_snapshotDir.c_str(), 0755);
    CLOG_INF("录像目录: %s\n", m_recordDir.c_str());
    CLOG_INF("拍照目录: %s\n", m_snapshotDir.c_str());

    // 注册 HTTP API：必须在 HttpServer Start() 之前/之后均可，路由表是独立的
    RegisterHttpApis();

    // ---- TLS / HTTPS / wss ----
    // 证书路径相对于 exe，由 sync.sh 推送到设备。
    // 注意：HTTP/WS 始终保留；HTTPS/WSS 只是"加一组监听端口"。
    // 若证书加载失败，程序仍可以通过 8080 / 8081 / 8082 提供明文服务，
    // 仅前端"讲话"按钮会因 location.protocol !== 'https:' 而隐藏。
    // 必须在 m_pHttpServer->Start() 之前 EnableTls，HttpServer 的 Start()
    // 会同时 bind/listen TLS 端口；WebSocketServer 的 AcceptThread 是循环检查
    // m_tlsServerFd，因此构造之后再 EnableTls 也能即时生效。
    {
        std::string crt = exeDir + "/cert/server.crt";
        std::string key = exeDir + "/cert/server.key";
        std::unique_ptr<C_TlsContext> tls(new C_TlsContext());
        if (tls->Init(crt, key)) {
            m_pTls = std::move(tls);
            m_pHttpServer  ->EnableTls(8443, m_pTls.get());
            m_pWsServer    ->EnableTls(8444, m_pTls.get());
            m_pWsFileServer->EnableTls(8445, m_pTls.get());
            CLOG_INF("HTTPS=8443  WSS-live=8444  WSS-playback=8445  cert=%s\n",
                     crt.c_str());
        } else {
            CLOG_INF("未找到 TLS 证书 (%s)，仅启用 HTTP/WS 明文模式；"
                     "讲话按钮在 HTTP 上不可用\n", crt.c_str());
        }
    }

    // 启动HTTP服务器
    m_pHttpServer->Start();

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
}


C_Terminal::~C_Terminal(){
    if (m_pVmd) m_pVmd->Stop();
    if (m_pPersonDetector) m_pPersonDetector->Stop();
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

void C_Terminal::TryInitMuxer()
{
    if (m_pMuxer) return;

    unsigned int spsPpsLen = 0;
    const unsigned char* spsPps = m_pH264Enc->GetSpsPps(&spsPpsLen);
    if (!spsPps || spsPpsLen == 0) {
        // SPS/PPS 还没准备好，等下一帧
        return;
    }

    unsigned int ascLen = 0;
    const unsigned char* asc = m_pAacEnc->GetAudioSpecificConfig(&ascLen);
    if (!asc || ascLen == 0) {
        // ASC 在 AacEnc 构造时即可生成，正常不会走到这里
        return;
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

    m_pMuxer.reset(new C_Fmp4Muxer(this, vcfg, acfg));
    CLOG_INF("fMP4 muxer 初始化完成 (sps/pps=%u, asc=%u)\n", spsPpsLen, ascLen);
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

    // 惰性初始化 muxer：等到拿到 SPS/PPS 后再建
    if (!m_pMuxer) {
        TryInitMuxer();
        if (!m_pMuxer) return 0;
    }

    // 入 muxer
    m_pMuxer->InputH264(data, dataLen, ptsUs, isKey);
    return 0;
}

//音频编码回调的aac数据
int C_Terminal::OnOutputAac(unsigned char* data, unsigned int dataLen, int64_t ptsUs)
{
    // muxer 还没建好（首个 IDR 还没出来）就先丢弃，避免无视频时单独出音频
    if (!m_pMuxer) return 0;

    m_pMuxer->InputAac(data, dataLen, ptsUs);
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
// 全部走 8080 端口；前端通过 fetch 调用，与 8081 (live ws) 互不影响。
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
} // namespace

void C_Terminal::RegisterHttpApis()
{
    auto* http = m_pHttpServer.get();

    // GET /api/record/status  ->  当前正在录的文件 / 字节数 / 根目录
    http->RegisterApi("GET", "/api/record/status",
        [this](const C_HttpServer::ApiRequest&) -> C_HttpServer::ApiResponse {
            C_HttpServer::ApiResponse rsp;
            std::ostringstream js;
            js << "{\"recording\":" << (m_pRecorder->IsRecording() ? "true" : "false")
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
}
