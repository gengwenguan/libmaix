/*********************************************************************************
  *Copyright(C),Your Company
  *FileName:  recordCleaner.cpp
  *Author:    gengwenguan
  *Date:      2026-05-23
  *Description:  录像目录后台清理实现，详见 recordCleaner.h
**********************************************************************************/
#include "recordCleaner.h"
#include "appConfig.h"
#include "logAdapt.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

C_RecordCleaner::C_RecordCleaner() = default;

C_RecordCleaner::~C_RecordCleaner()
{
    Stop();
}

int C_RecordCleaner::Start(const std::string& root, int intervalSec)
{
    if (m_thread.joinable()) {
        CLOG_INF("RecordCleaner already running\n");
        return 0;
    }
    m_root        = root;
    m_intervalSec = intervalSec > 0 ? intervalSec : 3600;
    m_stopFlag.store(false);
    m_thread = std::thread(&C_RecordCleaner::RunLoop, this);
    CLOG_INF("RecordCleaner started: root=%s interval=%ds (retain/maxBytes from AppConfig)\n",
             m_root.c_str(), m_intervalSec);
    return 0;
}

void C_RecordCleaner::Stop()
{
    if (!m_thread.joinable()) return;
    m_stopFlag.store(true);
    m_cv.notify_all();
    m_thread.join();
    CLOG_INF("RecordCleaner stopped\n");
}

void C_RecordCleaner::RunLoop()
{
    // 启动后立即扫一次，避免重启后旧文件长时间不被清
    while (!m_stopFlag.load()) {
        uint64_t r1 = SweepExpiredDirs();
        uint64_t r2 = SweepOverQuota();
        if (r1 + r2 > 0) {
            CLOG_INF("RecordCleaner cleaned: expired=%lluB overQuota=%lluB\n",
                     (unsigned long long)r1, (unsigned long long)r2);
        }
        std::unique_lock<std::mutex> lk(m_cvMu);
        m_cv.wait_for(lk, std::chrono::seconds(m_intervalSec),
                      [this]{ return m_stopFlag.load(); });
    }
}

bool C_RecordCleaner::IsDayDir(const std::string& name)
{
    if (name.size() != 8) return false;
    for (char c : name) if (c < '0' || c > '9') return false;
    return true;
}

int C_RecordCleaner::TodayInt()
{
    // 与 recorder 命名保持一致：UTC + 8h
    static constexpr int kChinaTimeOffsetSec = 8 * 60 * 60;
    std::time_t t = std::time(nullptr) + kChinaTimeOffsetSec;
    std::tm tmv{};
    localtime_r(&t, &tmv);
    return (tmv.tm_year + 1900) * 10000 + (tmv.tm_mon + 1) * 100 + tmv.tm_mday;
}

uint64_t C_RecordCleaner::DirSize(const std::string& path)
{
    DIR* d = opendir(path.c_str());
    if (!d) return 0;
    uint64_t total = 0;
    struct dirent* e = nullptr;
    while ((e = readdir(d)) != nullptr) {
        if (!std::strcmp(e->d_name, ".") || !std::strcmp(e->d_name, "..")) continue;
        std::string full = path + "/" + e->d_name;
        struct stat st{};
        if (stat(full.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
            total += (uint64_t)st.st_size;
        }
    }
    closedir(d);
    return total;
}

uint64_t C_RecordCleaner::TreeSize(const std::string& root)
{
    DIR* d = opendir(root.c_str());
    if (!d) return 0;
    uint64_t total = 0;
    struct dirent* e = nullptr;
    while ((e = readdir(d)) != nullptr) {
        if (!std::strcmp(e->d_name, ".") || !std::strcmp(e->d_name, "..")) continue;
        std::string full = root + "/" + e->d_name;
        struct stat st{};
        if (stat(full.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode))      total += DirSize(full);
        else if (S_ISREG(st.st_mode)) total += (uint64_t)st.st_size;
    }
    closedir(d);
    return total;
}

bool C_RecordCleaner::RmRf(const std::string& path)
{
    struct stat st{};
    if (lstat(path.c_str(), &st) != 0) return errno == ENOENT;
    if (S_ISDIR(st.st_mode)) {
        DIR* d = opendir(path.c_str());
        if (!d) return false;
        struct dirent* e = nullptr;
        bool ok = true;
        while ((e = readdir(d)) != nullptr) {
            if (!std::strcmp(e->d_name, ".") || !std::strcmp(e->d_name, "..")) continue;
            ok = RmRf(path + "/" + e->d_name) && ok;
        }
        closedir(d);
        if (rmdir(path.c_str()) != 0) ok = false;
        return ok;
    }
    return unlink(path.c_str()) == 0;
}

uint64_t C_RecordCleaner::SweepExpiredDirs()
{
    // 每次扫描时按需拉一次配置：避免在类内维护副本，web 改完最多等下个周期生效。
    // ClampSnapshot 已保证 record_retain_days ∈ [1, 365]，无需再校验下限。
    int retainDays = C_AppConfig::GetInst().GetSnapshot().record_retain_days;
    if (retainDays <= 0) return 0;
    DIR* d = opendir(m_root.c_str());
    if (!d) return 0;

    // 计算 cutoff：today - retainDays 之前的（< cutoff）整目录删
    // 与 recorder 命名保持一致：用 UTC+8 推算"今天"
    static constexpr int kChinaTimeOffsetSec = 8 * 60 * 60;
    std::time_t t = std::time(nullptr) + kChinaTimeOffsetSec;
    t -= (time_t)retainDays * 86400;
    std::tm tmv{};
    localtime_r(&t, &tmv);
    int cutoff = (tmv.tm_year + 1900) * 10000 + (tmv.tm_mon + 1) * 100 + tmv.tm_mday;

    uint64_t freed = 0;
    struct dirent* e = nullptr;
    while ((e = readdir(d)) != nullptr) {
        if (!IsDayDir(e->d_name)) continue;
        int day = std::atoi(e->d_name);
        if (day >= cutoff) continue;
        std::string full = m_root + "/" + e->d_name;
        uint64_t sz = DirSize(full);
        if (RmRf(full)) {
            CLOG_INF("RecordCleaner removed expired dir: %s (%lluB)\n",
                     full.c_str(), (unsigned long long)sz);
            freed += sz;
        }
    }
    closedir(d);
    return freed;
}

uint64_t C_RecordCleaner::SweepOverQuota()
{
    // 同上：现拉配置，避免成员副本。0 表示不限容量。
    uint64_t maxBytes = C_AppConfig::GetInst().GetSnapshot().record_max_bytes;
    if (maxBytes == 0) return 0;
    uint64_t total = TreeSize(m_root);
    if (total <= maxBytes) return 0;

    // 收集所有 .mp4 文件路径，按文件名（即时间戳）升序，从最老开始删
    std::vector<std::pair<std::string, uint64_t>> files;
    DIR* d = opendir(m_root.c_str());
    if (!d) return 0;
    struct dirent* e = nullptr;
    while ((e = readdir(d)) != nullptr) {
        if (!IsDayDir(e->d_name)) continue;
        std::string dayPath = m_root + "/" + e->d_name;
        DIR* dd = opendir(dayPath.c_str());
        if (!dd) continue;
        struct dirent* ee = nullptr;
        while ((ee = readdir(dd)) != nullptr) {
            std::string n(ee->d_name);
            if (n.size() < 5) continue;
            if (n.compare(n.size() - 4, 4, ".mp4") != 0) continue;
            std::string full = dayPath + "/" + n;
            struct stat st{};
            if (stat(full.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
                // 用 dayDir/filename 作为排序 key —— 字典序就是时间序
                files.emplace_back(std::string(e->d_name) + "/" + n, (uint64_t)st.st_size);
            }
        }
        closedir(dd);
    }
    closedir(d);

    std::sort(files.begin(), files.end(),
              [](const auto& a, const auto& b){ return a.first < b.first; });

    // 留 10% 余量，避免在阈值附近频繁抖动
    uint64_t target = (uint64_t)(maxBytes * 0.9);
    uint64_t freed = 0;
    for (auto& f : files) {
        if (total <= target) break;
        std::string full = m_root + "/" + f.first;
        if (unlink(full.c_str()) == 0) {
            total -= f.second;
            freed += f.second;
            CLOG_INF("RecordCleaner removed for quota: %s (%lluB)\n",
                     full.c_str(), (unsigned long long)f.second);
            // 顺手删伴生 .idx 索引文件（精准跳转用）。失败/不存在都安全忽略。
            std::string idx = full + ".idx";
            struct stat ist{};
            if (stat(idx.c_str(), &ist) == 0) {
                if (unlink(idx.c_str()) == 0) {
                    total = (total >= (uint64_t)ist.st_size) ? total - ist.st_size : 0;
                    freed += (uint64_t)ist.st_size;
                }
            }
        }
    }
    return freed;
}
