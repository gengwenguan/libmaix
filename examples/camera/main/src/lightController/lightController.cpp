/*********************************************************************************
  *Copyright(C),Your Company
  *FileName:  lightController.cpp
  *Author:    gengwenguan
  *Date:      2026-07-26
  *Description:  外接补光灯控制器实现。详见 lightController.h 顶部注释。
**********************************************************************************/
#include "lightController.h"
#include "appConfig.h"
#include "aacEnc.h"
#include "logAdapt.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

#include <fcntl.h>
#include <unistd.h>

namespace {

// 本项目对用户展示的时间统一使用北京时间（UTC+8）。板端实测无 /etc/localtime、
// 无 TZ，date 与 date -u 相同，直接 localtime_r() 得到 UTC。这里显式加 8h 后用
// gmtime_r() 拆分，避免未来系统配置了时区后又被 localtime_r() 重复偏移。
static constexpr time_t kChinaTimeOffsetSec = 8 * 60 * 60;

// 取当前北京时间小时 [0,23]。Unix time 本身始终是 UTC epoch；手动加固定偏移后
// 必须用 gmtime_r 拆分，结果不受进程 TZ / /etc/localtime 影响。
bool GetChinaHour(int& hour)
{
    time_t t = ::time(nullptr);
    if (t == (time_t)-1) return false;
    t += kChinaTimeOffsetSec;
    struct tm china{};
    if (!gmtime_r(&t, &china)) return false;
    hour = china.tm_hour;
    return true;
}

// 写一个只写小文件（sysfs export/direction/unexport）。成功返回 true。
bool WriteSysfs(const char* path, const char* val)
{
    int fd = ::open(path, O_WRONLY);
    if (fd < 0) return false;
    ssize_t n = ::write(fd, val, ::strlen(val));
    ::close(fd);
    return n >= 0;
}

long long NowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// 判断 hour 是否落在 [startH, endH) 时段内，支持跨零点。
// startH==endH 视为"全天"（24 小时都算在内）。
bool HourInWindow(int hour, int startH, int endH)
{
    if (startH == endH) return true;              // 全天
    if (startH < endH)  return hour >= startH && hour < endH;
    return hour >= startH || hour < endH;         // 跨零点，如 22 -> 6
}

} // namespace

C_LightController::~C_LightController()
{
    Stop();
}

void C_LightController::Start()
{
    bool expected = false;
    if (!m_running.compare_exchange_strong(expected, true,
                                           std::memory_order_acq_rel)) {
        return;   // 已在运行
    }
    try {
        m_thread = std::thread([this]() { this->Loop(); });
    } catch (...) {
        m_running.store(false, std::memory_order_release);
        CLOG_ERR("LightController: thread creation failed\n");
    }
}

void C_LightController::Stop()
{
    bool wasRunning = m_running.exchange(false, std::memory_order_acq_rel);
    if (!wasRunning) return;
    m_cv.notify_all();               // 唤醒可能在等待的评估线程
    if (m_thread.joinable()) m_thread.join();
    // 线程退出后由本函数统一收尾：灭灯 + 释放 GPIO（线程内不 release，避免竞态）。
    if (m_valueFd >= 0) WriteLevel(false);
    ReleaseGpio();
}

void C_LightController::Loop()
{
    CLOG_INF("LightController: loop start\n");
    while (m_running.load(std::memory_order_acquire)) {
        bool on = EvaluateShouldOn();
        if (m_valueFd >= 0) WriteLevel(on);

        // 每 500ms 评估一次：对"时段边界/声控保持到期"这类秒级事件足够灵敏，
        // 且几乎不占 CPU。用 cv 等待以便 Stop 能立即唤醒退出。
        std::unique_lock<std::mutex> lk(m_mu);
        m_cv.wait_for(lk, std::chrono::milliseconds(500), [this]() {
            return !m_running.load(std::memory_order_acquire);
        });
    }
    CLOG_INF("LightController: loop exit\n");
}

bool C_LightController::EvaluateShouldOn()
{
    const C_AppConfig::Snapshot cfg = C_AppConfig::GetInst().GetSnapshot();

    // 换脚 / 极性变化：现拉配置，必要时重建 GPIO（改完即时生效）。
    if (cfg.light_gpio != m_gpioNum || m_valueFd < 0) {
        if (!EnsureGpioReady(cfg.light_gpio)) return false;   // 就绪失败当作灭
    }
    m_activeLow = cfg.light_active_low;

    if (!cfg.light_enabled) return false;

    // 时段配置按北京时间（UTC+8）解释，与录像文件名、OSD、日志显示一致。
    // 取不到墙钟时间时保守不点亮。
    int chinaHour = 0;
    if (!GetChinaHour(chinaHour)) return false;
    if (!HourInWindow(chinaHour, cfg.light_start_hour, cfg.light_end_hour))
        return false;

    if (cfg.light_mode == 0) {
        // 时段内常亮
        return true;
    }

    // 声控：麦克风响度 >= 阈值时，把点亮截止时刻推到 now + hold 秒。
    int loud = AacEnc_GetMicLoudness();          // 0~100，无采集时为 0
    long long now = NowMs();
    if (loud >= cfg.light_sound_thresh) {
        long long holdMs = (long long)cfg.light_hold_s * 1000;
        m_soundHoldUntilMs = now + holdMs;
    }
    return now < m_soundHoldUntilMs;
}

bool C_LightController::EnsureGpioReady(int gpioNum)
{
    if (gpioNum == m_gpioNum && m_valueFd >= 0) return true;
    ReleaseGpio();   // 换脚：先释放旧的

    if (gpioNum < 0) return false;

    char buf[64];
    // 1) export（已导出时 write 失败，忽略即可）
    std::snprintf(buf, sizeof(buf), "%d", gpioNum);
    WriteSysfs("/sys/class/gpio/export", buf);

    // 2) direction=out（初始 low，避免上电瞬间误亮）
    char dirPath[96];
    std::snprintf(dirPath, sizeof(dirPath),
                  "/sys/class/gpio/gpio%d/direction", gpioNum);
    if (!WriteSysfs(dirPath, "low")) {
        CLOG_ERR("LightController: set direction failed for gpio%d\n", gpioNum);
        return false;
    }

    // 3) 持久打开 value fd
    char valPath[96];
    std::snprintf(valPath, sizeof(valPath),
                  "/sys/class/gpio/gpio%d/value", gpioNum);
    int fd = ::open(valPath, O_WRONLY);
    if (fd < 0) {
        CLOG_ERR("LightController: open value failed for gpio%d\n", gpioNum);
        return false;
    }
    m_valueFd = fd;
    m_gpioNum = gpioNum;
    m_curLevel = -1;    // 强制下次 WriteLevel 真正写一次
    CLOG_INF("LightController: gpio%d ready (active_%s)\n",
             gpioNum, m_activeLow ? "low" : "high");
    return true;
}

void C_LightController::ReleaseGpio()
{
    if (m_valueFd >= 0) { ::close(m_valueFd); m_valueFd = -1; }
    if (m_gpioNum >= 0) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%d", m_gpioNum);
        WriteSysfs("/sys/class/gpio/unexport", buf);
        m_gpioNum = -1;
    }
    m_curLevel = -1;
}

void C_LightController::WriteLevel(bool on)
{
    if (m_valueFd < 0) return;
    int level = on ? 1 : 0;
    if (level == m_curLevel) return;         // 去抖：电平未变不重复写
    // active_low 灯板：逻辑"亮"对应物理低电平。
    const char* v = (on != m_activeLow) ? "1" : "0";
    if (::write(m_valueFd, v, 1) == 1) {
        m_curLevel = level;
    }
}
