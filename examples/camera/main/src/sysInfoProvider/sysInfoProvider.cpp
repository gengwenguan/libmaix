/*********************************************************************************
  *Copyright(C),Your Company
  *FileName:  sysInfoProvider.cpp
  *Author:    gengwenguan
  *Date:      2026-07-19
  *Description:  设备系统信息采集实现。详见 sysInfoProvider.h。
**********************************************************************************/
#include "sysInfoProvider.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/statvfs.h>
#include <unistd.h>

namespace {

// 读一个小文本文件的全部内容（procfs / sysfs 都很小）。失败返回空串。
std::string ReadAll(const char* path)
{
    std::ifstream ifs(path);
    if (!ifs.is_open()) return std::string();
    std::ostringstream ss;
    ss << ifs.rdbuf();
    return ss.str();
}

// 从 "Key:  value kB" 形式的 meminfo 文本里取某行的数值（KB）。找不到返回 false。
bool GrepKvKb(const std::string& text, const char* key, uint64_t& out)
{
    size_t klen = std::strlen(key);
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        // 行首匹配 key
        if (text.compare(pos, klen, key) == 0) {
            const char* p = text.c_str() + pos + klen;
            // 跳过 ':' 和空白
            while (*p == ':' || *p == ' ' || *p == '\t') ++p;
            char* end = nullptr;
            unsigned long long v = std::strtoull(p, &end, 10);
            if (end != p) { out = (uint64_t)v; return true; }
        }
        pos = eol + 1;
    }
    return false;
}

} // namespace

C_SysInfoProvider::CpuTimes C_SysInfoProvider::ReadCpuTimes()
{
    CpuTimes t;
    std::string s = ReadAll("/proc/stat");
    if (s.empty()) return t;
    // 首行："cpu  user nice system idle iowait irq softirq steal guest guest_nice"
    if (s.compare(0, 4, "cpu ") != 0 && s.compare(0, 4, "cpu\t") != 0) return t;
    std::istringstream is(s);
    std::string cpuLabel;
    is >> cpuLabel;   // "cpu"
    uint64_t vals[10] = {0};
    int n = 0;
    for (; n < 10; ++n) {
        if (!(is >> vals[n])) break;
    }
    if (n < 4) return t;   // 至少要有 user/nice/system/idle
    uint64_t idle = vals[3] + (n > 4 ? vals[4] : 0);   // idle + iowait
    uint64_t total = 0;
    for (int i = 0; i < n; ++i) total += vals[i];
    t.total = total;
    t.idle  = idle;
    t.valid = true;
    return t;
}

C_SysInfoProvider::Info C_SysInfoProvider::Sample(const std::string& diskPath,
                                                  uint64_t watchdogThresholdKb)
{
    Info info;

    // ---- CPU 使用率：两次采样求差 ----
    {
        CpuTimes cur = ReadCpuTimes();
        std::lock_guard<std::mutex> lk(m_mu);
        if (cur.valid && m_lastCpu.valid) {
            uint64_t dTotal = (cur.total >= m_lastCpu.total) ? cur.total - m_lastCpu.total : 0;
            uint64_t dIdle  = (cur.idle  >= m_lastCpu.idle)  ? cur.idle  - m_lastCpu.idle  : 0;
            if (dTotal > 0) {
                double busy = (double)(dTotal - dIdle) / (double)dTotal * 100.0;
                if (busy < 0) busy = 0;
                if (busy > 100) busy = 100;
                info.cpuPercent = busy;
                info.cpuValid = true;
            }
        }
        if (cur.valid) m_lastCpu = cur;
    }
    long cores = sysconf(_SC_NPROCESSORS_ONLN);
    if (cores > 0) info.cpuCores = (int)cores;

    // ---- 负载均衡 /proc/loadavg ----
    {
        std::string s = ReadAll("/proc/loadavg");
        if (!s.empty()) {
            std::istringstream is(s);
            if (is >> info.load1 >> info.load5 >> info.load15) info.loadValid = true;
        }
    }

    // ---- 系统内存 /proc/meminfo ----
    {
        std::string s = ReadAll("/proc/meminfo");
        uint64_t total = 0, avail = 0;
        if (!s.empty() && GrepKvKb(s, "MemTotal", total)) {
            info.memTotalKb = total;
            // MemAvailable 更准（含可回收缓存）；老内核没有则回退 MemFree
            if (GrepKvKb(s, "MemAvailable", avail) || GrepKvKb(s, "MemFree", avail)) {
                info.memAvailKb = avail;
            }
            info.memValid = (total > 0);
        }
    }

    // ---- 本进程内存 /proc/self/status ----
    {
        std::string s = ReadAll("/proc/self/status");
        uint64_t rss = 0, data = 0;
        if (!s.empty()) {
            bool hasRss  = GrepKvKb(s, "VmRSS", rss);
            bool hasData = GrepKvKb(s, "VmData", data);
            if (hasRss)  info.procVmRssKb  = rss;
            if (hasData) info.procVmDataKb = data;
            info.procValid = (hasRss || hasData);
            info.watchdogThresholdKb = watchdogThresholdKb;
        }
    }

    // ---- 磁盘 statvfs（录像所在分区）----
    if (!diskPath.empty()) {
        struct statvfs vfs;
        if (statvfs(diskPath.c_str(), &vfs) == 0 && vfs.f_blocks > 0) {
            uint64_t bsize = (vfs.f_frsize ? vfs.f_frsize : vfs.f_bsize);
            info.diskTotalBytes = (uint64_t)vfs.f_blocks * bsize;
            info.diskAvailBytes = (uint64_t)vfs.f_bavail * bsize;   // 非特权可用
            info.diskValid = true;
        }
    }

    // ---- 运行时长 /proc/uptime ----
    double sysUptime = -1.0;
    {
        std::string s = ReadAll("/proc/uptime");
        if (!s.empty()) {
            double up = 0;
            std::istringstream is(s);
            if (is >> up && up >= 0) {
                sysUptime = up;
                info.uptimeSec = (uint64_t)up; info.uptimeValid = true;
            }
        }
    }

    // ---- 本进程运行时长：系统 uptime - 进程 starttime（/proc/self/stat 第22字段）----
    // starttime 单位是时钟节拍(clock ticks since boot)，除以 _SC_CLK_TCK 得秒。
    // comm(第2字段)可能含空格/括号，必须从最后一个 ')' 之后开始按空格切字段。
    if (sysUptime >= 0) {
        std::string s = ReadAll("/proc/self/stat");
        size_t rparen = s.rfind(')');
        if (!s.empty() && rparen != std::string::npos) {
            // 从 ')' 之后是 field 3(state) 起。field 22(starttime) = ')' 之后的第 20 个 token。
            std::istringstream is(s.substr(rparen + 1));
            std::string tok;
            long idx = 2;   // 已越过 field 1(pid)、2(comm)
            unsigned long long starttimeTicks = 0;
            bool got = false;
            while (is >> tok) {
                ++idx;
                if (idx == 22) {
                    char* end = nullptr;
                    starttimeTicks = std::strtoull(tok.c_str(), &end, 10);
                    got = (end != tok.c_str());
                    break;
                }
            }
            long hz = sysconf(_SC_CLK_TCK);
            if (got && hz > 0) {
                double startedAt = (double)starttimeTicks / (double)hz;  // 进程启动时的 boot 秒数
                double procUp = sysUptime - startedAt;
                if (procUp < 0) procUp = 0;
                info.procUptimeSec = (uint64_t)procUp;
                info.procUptimeValid = true;
            }
        }
    }

    return info;
}
