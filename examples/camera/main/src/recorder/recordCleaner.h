/*********************************************************************************
  *Copyright(C),Your Company
  *FileName:  recordCleaner.h
  *Author:    gengwenguan
  *Date:      2026-05-23
  *Description:  录像目录后台清理（保留天数 + 总容量双兜底）。
  *
  *  策略：
  *    - 每 m_intervalSec 秒扫一遍 root 下所有 YYYYMMDD 子目录
  *    - 先按"日期 < cutoffDate"的目录整体 rm -rf，cutoffDate = today - retainDays
  *    - 再统计剩余容量，若 > maxBytes，从最老的文件开始 unlink 直到总量 <= maxBytes*0.9（留 10% 余量避免抖动）
  *    - 不会触碰当前正在写的文件（按 wallclock 文件名排序，最新的留在最后处理）
  *
  *  配置来源：
  *    retainDays / maxBytes 不再保存为成员变量，每次扫描时从 C_AppConfig 拉取，
  *    web 改完配置最多等下一个扫描周期生效（默认 1 小时一次，对监控场景可接受）。
  *    省去了 setter / atomic / Subscribe 回调一整套同步机制。
  *
  *  线程模型：
  *    一个后台 std::thread 跑 RunLoop()，析构时 stopFlag=true + cv.notify。
**********************************************************************************/
#pragma once
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

class C_RecordCleaner
{
public:
    C_RecordCleaner();
    ~C_RecordCleaner();

    // root: 录像根目录；intervalSec: 扫描周期（秒），默认 3600。
    // retainDays / maxBytes 由 AppConfig 提供，无需在此传入。
    int Start(const std::string& root, int intervalSec = 3600);
    void Stop();

private:
    void RunLoop();
    // 删过期目录；返回删除的字节数。每次调用时从 AppConfig 现拉 retainDays。
    uint64_t SweepExpiredDirs();
    // 容量水位裁剪；返回删除的字节数。每次调用时从 AppConfig 现拉 maxBytes。
    uint64_t SweepOverQuota();

    static bool   RmRf(const std::string& path);
    static bool   IsDayDir(const std::string& name);  // YYYYMMDD 校验
    static int    TodayInt();                          // 返回今天的 YYYYMMDD 数字
    static uint64_t DirSize(const std::string& path);
    static uint64_t TreeSize(const std::string& root);

private:
    std::string  m_root;
    int          m_intervalSec = 3600;

    std::atomic<bool>       m_stopFlag{false};
    std::thread             m_thread;
    std::mutex              m_cvMu;
    std::condition_variable m_cv;
};
