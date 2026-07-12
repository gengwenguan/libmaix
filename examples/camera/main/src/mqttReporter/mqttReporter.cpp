/*********************************************************************************
  *Copyright(C),Your Company
  *FileName:  mqttReporter.cpp
  *Author:    gengwenguan
  *Date:      2026-07-12
  *Description:  IPv6 地址变化 MQTT 上报模块实现。详见 mqttReporter.h。
**********************************************************************************/
#include "mqttReporter.h"
#include "mqttClient.h"
#include "appConfig.h"
#include "logAdapt.h"

#include <chrono>
#include <cstdint>
#include <cstring>

#include <ifaddrs.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

namespace {
inline int64_t NowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
} // namespace

C_MqttReporter::C_MqttReporter() {}

C_MqttReporter::~C_MqttReporter()
{
    Stop();
}

int C_MqttReporter::Start()
{
    if (m_running.exchange(true)) return 0;   // 已在运行
    try {
        m_thread = std::thread(&C_MqttReporter::PollLoop, this);
    } catch (...) {
        m_running = false;
        CLOG_ERR("MqttReporter: start thread failed\n");
        return -1;
    }
    CLOG_INF("MqttReporter: started\n");
    return 0;
}

void C_MqttReporter::Stop()
{
    if (!m_running.exchange(false)) return;   // 未运行 / 已停
    m_cv.notify_all();                         // 打断定时睡眠
    if (m_thread.joinable()) m_thread.join();
    CLOG_INF("MqttReporter: stopped\n");
}

// 取指定网卡上第一个全局单播 IPv6（排除 link-local fe80:: 与回环 ::1）
std::string C_MqttReporter::GetGlobalIpv6(const std::string& iface)
{
    struct ifaddrs* ifaddr = nullptr;
    if (getifaddrs(&ifaddr) == -1) {
        CLOG_ERR("MqttReporter: getifaddrs failed\n");
        return "";
    }

    std::string result;
    for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr) continue;
        if (ifa->ifa_addr->sa_family != AF_INET6) continue;
        if (!iface.empty() && iface != (ifa->ifa_name ? ifa->ifa_name : "")) continue;

        const struct sockaddr_in6* sa6 =
            reinterpret_cast<const struct sockaddr_in6*>(ifa->ifa_addr);
        const struct in6_addr* a6 = &sa6->sin6_addr;

        // 排除 link-local（fe80::/10）与回环（::1）
        if (IN6_IS_ADDR_LINKLOCAL(a6)) continue;
        if (IN6_IS_ADDR_LOOPBACK(a6))  continue;

        char host[NI_MAXHOST] = {0};
        int s = getnameinfo(ifa->ifa_addr, sizeof(struct sockaddr_in6),
                            host, sizeof(host), nullptr, 0, NI_NUMERICHOST);
        if (s != 0) {
            CLOG_ERR("MqttReporter: getnameinfo failed: %s\n", gai_strerror(s));
            continue;
        }
        // 去掉可能的 "%scope" 后缀（全局地址一般没有，稳妥处理）
        std::string ip(host);
        size_t pct = ip.find('%');
        if (pct != std::string::npos) ip = ip.substr(0, pct);

        result = ip;
        break;   // 取第一个即可
    }

    freeifaddrs(ifaddr);
    return result;
}

void C_MqttReporter::PollLoop()
{
    while (m_running.load()) {
        C_AppConfig::Snapshot cfg = C_AppConfig::GetInst().GetSnapshot();

        if (cfg.mqtt_enabled) {
            std::string ip = GetGlobalIpv6(cfg.mqtt_iface);
            if (ip.empty()) {
                CLOG_INF("MqttReporter: no global IPv6 on iface=%s yet\n",
                         cfg.mqtt_iface.c_str());
            } else {
                bool changed = (ip != m_lastIpv6);
                // 保活重报：interval>0 且距上次成功上报已超过该间隔，即使地址没变也重报
                bool keepalive = false;
                if (cfg.mqtt_report_interval_s > 0 && m_lastPublishMs != 0) {
                    int64_t elapsedMs = NowMs() - m_lastPublishMs;
                    keepalive = elapsedMs >= (int64_t)cfg.mqtt_report_interval_s * 1000;
                }
                if (changed || keepalive) {
                    const char* reason = changed ? "changed" : "keepalive";
                    CLOG_INF("MqttReporter: IPv6 %s '%s' -> '%s', publishing to %s:%d topic=%s\n",
                             reason, m_lastIpv6.c_str(), ip.c_str(),
                             cfg.mqtt_broker_host.c_str(), cfg.mqtt_broker_port,
                             cfg.mqtt_topic.c_str());
                    bool ok = MqttClient::PublishOnce(
                        cfg.mqtt_broker_host, cfg.mqtt_broker_port,
                        cfg.mqtt_client_id, cfg.mqtt_topic, ip,
                        cfg.mqtt_retain);
                    if (ok) {
                        m_lastIpv6 = ip;             // 成功才更新缓存
                        m_lastPublishMs = NowMs();   // 重置保活计时
                        CLOG_INF("MqttReporter: publish ok\n");
                    } else {
                        CLOG_ERR("MqttReporter: publish failed, will retry next round\n");
                        // 不更新 m_lastIpv6 / m_lastPublishMs，下一轮仍会重试
                    }
                }
                // 地址没变且未到保活周期：什么都不做（稳态低开销）
            }
        }

        // 可被 Stop() 立即打断的定时睡眠
        int waitSec = cfg.mqtt_poll_sec > 0 ? cfg.mqtt_poll_sec : 10;
        std::unique_lock<std::mutex> lk(m_cvMu);
        m_cv.wait_for(lk, std::chrono::seconds(waitSec),
                      [this] { return !m_running.load(); });
    }
}
