/*********************************************************************************
  *Copyright(C),Your Company
  *FileName:  sysInfoProvider.h
  *Author:    gengwenguan
  *Date:      2026-07-19
  *Description:  设备系统信息采集：CPU / 内存 / 进程内存(VmData) / 磁盘 /
  *             运行时长。全部读 Linux procfs + statvfs，供 web「设备状态」卡片展示。
  *
  *  为什么单开模块：
  *    这些指标与业务无关、纯只读采集，抽出来便于复用与单测；CPU 使用率需要
  *    "两次 /proc/stat 采样求差"，因此需要一个持有上次采样的对象（非纯静态函数）。
  *
  *  线程模型：
  *    仅 HTTP 客户端线程在 /api/sysinfo 里调用 Sample()。多个并发请求可能同时调，
  *    故 Sample() 内部用 m_mu 串行保护上次 CPU 采样，保证增量计算正确。
  *
  *  进程内存(VmData)：
  *    对齐 mem_watchdog.sh —— 它监控的正是 camera 进程 /proc/<pid>/status 的
  *    VmData（闭源库慢泄漏体现在此），到 40MB 阈值就重启。这里一并暴露，让 web
  *    能直观看到"离看门狗重启还有多远"。
**********************************************************************************/
#pragma once
#include <cstdint>
#include <mutex>
#include <string>

class C_SysInfoProvider
{
public:
    struct Info {
        // CPU：总使用率（0~100，两次采样差值）；首次调用无历史样本 → cpuValid=false
        bool     cpuValid   = false;
        double   cpuPercent = 0.0;
        int      cpuCores   = 0;
        // 负载均衡（/proc/loadavg 的 1/5/15 分钟）
        bool     loadValid  = false;
        double   load1 = 0.0, load5 = 0.0, load15 = 0.0;
        // 系统内存（KB，来自 /proc/meminfo）
        bool     memValid   = false;
        uint64_t memTotalKb = 0, memAvailKb = 0;   // used = total - avail
        // 本进程内存（KB，/proc/self/status 的 VmRSS 与 VmData）
        bool     procValid  = false;
        uint64_t procVmRssKb = 0, procVmDataKb = 0;
        uint64_t watchdogThresholdKb = 0;          // mem_watchdog 阈值（0=未设）
        // 磁盘：录像所在分区（字节，statvfs）
        bool     diskValid  = false;
        uint64_t diskTotalBytes = 0, diskAvailBytes = 0;
        // 系统运行时长（秒，/proc/uptime）
        bool     uptimeValid = false;
        uint64_t uptimeSec = 0;
        // 本进程（camera）运行时长（秒）：系统 uptime - 进程 starttime。
        // 反映"距上次 mem_watchdog 重启过了多久"，比系统 uptime 更贴合运维关注点。
        bool     procUptimeValid = false;
        uint64_t procUptimeSec = 0;
    };

    C_SysInfoProvider() = default;

    // 采集一次。diskPath：用于 statvfs 的任意路径（传录像目录即可）。
    // watchdogThresholdKb：mem_watchdog 阈值，用于在结果里回显（传 0 表示不回显）。
    Info Sample(const std::string& diskPath, uint64_t watchdogThresholdKb = 0);

private:
    // /proc/stat 的 cpu 汇总行累计 jiffies；用于两次采样求 CPU 使用率。
    struct CpuTimes { uint64_t total = 0; uint64_t idle = 0; bool valid = false; };
    static CpuTimes ReadCpuTimes();

    std::mutex m_mu;
    CpuTimes   m_lastCpu;   // 上次采样，Sample() 内加锁读写
};
