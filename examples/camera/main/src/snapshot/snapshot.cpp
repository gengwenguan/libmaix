/*********************************************************************************
  *Copyright(C),Your Company
  *FileName:  snapshot.cpp
  *Author:    gengwenguan
  *Date:      2026-05-23
  *Description:  手动拍照实现。
  *
  *  历史：原来用 FFmpeg MJPEG encoder，在本平台上 avcodec_send_frame 内部稳定
  *       segfault（多种像素格式 / QSCALE 配置都试过）。
  *       改为 OpenCV cv::imencode("*.jpg")：
  *         - main.cpp 里已经在用 cv::Mat 做 OSD，证明 OpenCV 在该平台稳定
  *         - imencode 内部用 libjpeg-turbo 或 libjpeg，不走 ffmpeg
  *         - 直接 NV21 → BGR (cv::cvtColor) → JPEG bytes，路径短不易踩坑
**********************************************************************************/
#include "snapshot.h"
#include "appConfig.h"
#include "logAdapt.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <chrono>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>

// 中国时区偏移量（UTC+8）。开发板可能没配 /etc/localtime，
// 所以这里参考 logAdapt.cpp 的做法：先给 time_t 加 8h，再 localtime_r。
static constexpr int kChinaTimeOffsetSec = 8 * 60 * 60;
static inline std::tm GetChinaLocalTime()
{
    using namespace std::chrono;
    std::time_t t = system_clock::to_time_t(system_clock::now()) + kChinaTimeOffsetSec;
    std::tm tmv{};
    localtime_r(&t, &tmv);
    return tmv;
}

C_Snapshot::C_Snapshot(int width, int height)
    : m_width(width), m_height(height)
{
    // m_pending 不在构造时分配；首次"被请求"时才 resize，避免常驻 460KB 内存。
    CLOG_INF("snapshot: ready %dx%d (opencv jpeg, on-demand grab)\n", m_width, m_height);
}

C_Snapshot::~C_Snapshot()
{
}

bool C_Snapshot::MkdirP(const std::string& path)
{
    if (path.empty()) return false;
    std::string p; p.reserve(path.size());
    for (size_t i = 0; i < path.size(); ++i) {
        char c = path[i];
        p += c;
        if (c == '/' && i > 0) {
            mkdir(p.c_str(), 0755); // 忽略 EEXIST
        }
    }
    if (path.back() != '/') {
        if (mkdir(path.c_str(), 0755) != 0) {
            struct stat st{};
            if (stat(path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return false;
        }
    }
    return true;
}

int C_Snapshot::Start(const std::string& outDir)
{
    m_outDir = outDir;
    if (!MkdirP(m_outDir)) {
        CLOG_ERR("snapshot: mkdir %s failed\n", m_outDir.c_str());
        return -1;
    }
    CLOG_INF("snapshot: out dir = %s\n", m_outDir.c_str());
    return 0;
}

void C_Snapshot::OnNv21Frame(const unsigned char* nv21)
{
    // ---- 快路径 ----
    // 没人在等帧时只做一次原子 load 立即返回。
    // memory_order_relaxed 足够：我们不需要看到 TakeOne 之前写的别的内存，只关心
    // "want 这一位"自身的可见性；即便偶尔晚一帧拿到 want=true，也只是把当前帧让给
    // 下一帧来抓，不会出错。
    if (!m_want.load(std::memory_order_relaxed)) return;
    if (!nv21) return;

    // ---- 慢路径：有 TakeOne 在挂起，才真正 memcpy ----
    const size_t n = (size_t)m_width * m_height * 3 / 2;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        // 双重检查：拿到锁后可能 TakeOne 已经超时撤销了请求
        if (!m_want.load(std::memory_order_relaxed)) return;
        if (m_pending.size() != n) m_pending.assign(n, 0);
        std::memcpy(m_pending.data(), nv21, n);
        m_haveFrame = true;
        // 清掉请求位：本帧已应答；下一次 TakeOne 才会再次置位
        m_want.store(false, std::memory_order_relaxed);
    }
    m_cv.notify_all();
}

bool C_Snapshot::EncodeNv21ToJpeg(const std::vector<unsigned char>& nv21,
                                  std::vector<unsigned char>& jpeg)
{
    const int yW = m_width, yH = m_height;
    const size_t expectN = (size_t)yW * yH * 3 / 2;
    if (nv21.size() < expectN) {
        CLOG_ERR("snapshot: nv21 buffer too small (%zu < %zu)\n", nv21.size(), expectN);
        return false;
    }

    // 1. NV21 → BGR：OpenCV 直接接收 NV21 buffer 视图（H*3/2 行 × W 字节）
    //    cv::Mat 包裹但不拷贝；cvtColor 会输出独立的 BGR Mat
    cv::Mat nv21Mat(yH * 3 / 2, yW, CV_8UC1,
                    const_cast<unsigned char*>(nv21.data()));
    cv::Mat bgr;
    cv::cvtColor(nv21Mat, bgr, cv::COLOR_YUV2BGR_NV21);

    // 2. 编码到内存。质量来自 AppConfig，允许用户在设置面板里微调。
    int q = C_AppConfig::GetInst().GetSnapshot().photo_jpeg_qual;
    std::vector<int> params{ cv::IMWRITE_JPEG_QUALITY, q };
    bool ok = cv::imencode(".jpg", bgr, jpeg, params);
    if (!ok || jpeg.empty()) {
        CLOG_ERR("snapshot: cv::imencode jpg failed\n");
        return false;
    }
    return true;
}

std::string C_Snapshot::TakeOne(std::string* outFullPath)
{
    if (m_outDir.empty()) {
        CLOG_ERR("snapshot: not started\n");
        return std::string();
    }

    // ---- 1. 请求一帧 ----
    // 置位 m_want，相机线程下次 OnNv21Frame 看到就会 memcpy 一份并 notify。
    // 用 unique_lock + cv.wait_for 实现 1s 超时（兜底：万一相机线程卡住也不会
    // 永远阻塞 HTTP / detector）。
    std::vector<unsigned char> localFrame;   // 本地承接帧；swap 出锁外编码
    {
        std::unique_lock<std::mutex> lk(m_mu);
        m_haveFrame = false;                 // 清旧标志，确保等的是"新一帧"
        m_want.store(true, std::memory_order_relaxed);
        bool ok = m_cv.wait_for(lk, std::chrono::milliseconds(1000),
                                [this]{ return m_haveFrame; });
        if (!ok) {
            // 超时：撤销请求位并返回空
            m_want.store(false, std::memory_order_relaxed);
            CLOG_ERR("snapshot: timeout waiting for frame\n");
            return std::string();
        }
        // 把缓冲 swap 出来，锁外做 cvtColor + jpeg encode（耗时 10~30ms）
        localFrame.swap(m_pending);
        m_haveFrame = false;
    }

    // ---- 2. 锁外编码 ----
    std::vector<unsigned char> jpeg;
    if (!EncodeNv21ToJpeg(localFrame, jpeg)) {
        CLOG_ERR("snapshot: encode jpeg failed\n");
        return std::string();
    }

    // ---- 3. 命名 + 写盘 ----
    std::tm tmv = GetChinaLocalTime();
    int day = (tmv.tm_year + 1900) * 10000 + (tmv.tm_mon + 1) * 100 + tmv.tm_mday;
    {
        // 同日序号自增；跨日重置（独立小锁，与帧 buffer 锁无关）
        std::lock_guard<std::mutex> lk(m_seqMu);
        if (day != m_lastDay) { m_lastDay = day; m_seqOfDay = 0; }
        m_seqOfDay++;
    }
    char name[64] = {0};
    std::snprintf(name, sizeof(name), "%04d%02d%02d_%02d%02d%02d_%03d.jpg",
                  tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                  tmv.tm_hour, tmv.tm_min, tmv.tm_sec, m_seqOfDay);

    std::string full = m_outDir + "/" + name;
    std::ofstream ofs(full, std::ios::binary);
    if (!ofs.is_open()) {
        CLOG_ERR("snapshot: open %s failed\n", full.c_str());
        return std::string();
    }
    ofs.write(reinterpret_cast<const char*>(jpeg.data()), (std::streamsize)jpeg.size());
    ofs.close();
    if (outFullPath) *outFullPath = full;
    CLOG_INF("snapshot: saved %s (%zu bytes)\n", name, jpeg.size());

    // 写盘成功后顺手做一次淘汰。每次只删极少量文件（通常 0~1 张），开销可忽略。
    int maxCnt = C_AppConfig::GetInst().GetSnapshot().album_max_photos;
    if (maxCnt > 0) PruneOldestIfOver(maxCnt);

    return std::string(name);
}

int C_Snapshot::PruneOldestIfOver(int maxCount)
{
    if (m_outDir.empty() || maxCount <= 0) return 0;

    // 1. 列出 outDir 下所有 .jpg：(mtime, fullPath)
    std::vector<std::pair<int64_t, std::string>> items;
    DIR* d = opendir(m_outDir.c_str());
    if (!d) {
        CLOG_ERR("snapshot prune: opendir %s failed\n", m_outDir.c_str());
        return 0;
    }
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        if (ent->d_name[0] == '.') continue;
        size_t L = std::strlen(ent->d_name);
        if (L < 4) continue;
        if (std::strcmp(ent->d_name + L - 4, ".jpg") != 0) continue;
        std::string full = m_outDir + "/" + ent->d_name;
        struct stat st{};
        if (stat(full.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
        items.emplace_back((int64_t)st.st_mtime, std::move(full));
    }
    closedir(d);

    if ((int)items.size() <= maxCount) return 0;

    // 2. 按 mtime 升序排（旧的在前），删除最旧 N 张
    std::sort(items.begin(), items.end(),
              [](const auto& a, const auto& b){ return a.first < b.first; });

    int toDel = (int)items.size() - maxCount;
    int delOk = 0;
    for (int i = 0; i < toDel; ++i) {
        const std::string& path = items[i].second;
        if (unlink(path.c_str()) == 0) {
            ++delOk;
        } else {
            CLOG_ERR("snapshot prune: unlink %s failed: %s\n",
                     path.c_str(), std::strerror(errno));
        }
    }
    if (delOk > 0) {
        CLOG_INF("snapshot prune: deleted %d oldest photos (cap=%d)\n", delOk, maxCount);
    }
    return delOk;
}
