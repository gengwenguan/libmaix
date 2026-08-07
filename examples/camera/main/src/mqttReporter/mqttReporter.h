/*********************************************************************************
  *Copyright(C),Your Company
  *FileName:  mqttReporter.h
  *Author:    gengwenguan
  *Date:      2026-07-12
  *Description:  IPv6 地址变化 MQTT 上报模块。
  *
  *  背景：M2dock（V831）wlan0 的公网 IPv6 由运营商动态下发、会不定期变化。
  *        外部要通过 IPv6 直连板子 web 服务（80/443）就得知道当前地址。
  *
  *  职责：常驻后台线程，定时轮询指定网卡（默认 wlan0）的"全局 IPv6 地址"，
  *        一旦相较上次发生变化，就用极简 MQTT 客户端（mqttClient）把新地址
  *        以纯字符串 payload 发布到配置的 topic。
  *
  *  设计（对齐本工程既有 motionDetector 线程范式 + esp32 样例 supervisor 思路）：
  *    - Start()/Stop()：拉起 / 停止后台线程，Stop 可重入；用 condition_variable
  *      做"可被 Stop 立即打断"的定时睡眠。
  *    - 配置 pull-on-demand：每轮从 C_AppConfig::GetSnapshot() 现取 broker/topic/
  *      poll_sec/iface 等，web 改完下一轮即生效；mqtt_enabled=false 时不采集不连网，
  *      仅空转睡眠（近零开销，语义同 VMD 关闭）。
  *    - 连接策略：按需短连接——只有检测到 IPv6 变化才连 broker，publish 完即断。
  *    - 异常：publish 失败不更新缓存，下一轮自然重试。
  *
  *  线程安全：m_lastIpv6 仅本线程读写，无需加锁。
**********************************************************************************/
#pragma once
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

class C_MqttReporter
{
public:
    C_MqttReporter();
    ~C_MqttReporter();

    // 启动后台轮询线程。线程启动失败返回非 0。
    int  Start();
    // 停止线程并清理。可重入。
    void Stop();

    // 取指定网卡上第一个"全局单播 IPv6"地址（排除 fe80:: link-local、::1 回环）。
    // 取不到返回空串。返回值已去掉可能的 "%scope" 后缀。
    // 这是无状态的网卡查询工具，同时供 MQTT 上报与 HTTP /api/netinfo 复用。
    static std::string GetGlobalIpv6(const std::string& iface);

private:
    void PollLoop();

    std::thread             m_thread;
    std::atomic<bool>       m_running{false};
    std::mutex              m_cvMu;
    std::condition_variable m_cv;

    // 上次成功上报的 IPv6；空串表示尚未上报过（冷启动首次拿到即视为变化）
    std::string             m_lastIpv6;
    // 上次成功上报的单调时钟毫秒；用于保活重报判定（0 表示尚未报过）
    int64_t                 m_lastPublishMs = 0;
};
