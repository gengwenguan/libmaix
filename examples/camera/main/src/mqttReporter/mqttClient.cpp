/*********************************************************************************
  *Copyright(C),Your Company
  *FileName:  mqttClient.cpp
  *Author:    gengwenguan
  *Date:      2026-07-12
  *Description:  极简 MQTT 3.1.1 QoS0 发布客户端实现。详见 mqttClient.h。
**********************************************************************************/
#include "mqttClient.h"
#include "logAdapt.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace {

// MQTT 报文类型（固定头高 4 位）
constexpr uint8_t kConnect    = 0x10;
constexpr uint8_t kConnack    = 0x20;
constexpr uint8_t kPublish    = 0x30;   // QoS0 / no dup / no retain
constexpr uint8_t kDisconnect = 0xE0;

// 变长 Remaining Length 编码（MQTT 3.1.1，最多 4 字节）
void EncodeRemainingLength(std::vector<uint8_t>& out, uint32_t len)
{
    do {
        uint8_t b = len % 128;
        len /= 128;
        if (len > 0) b |= 0x80;
        out.push_back(b);
    } while (len > 0);
}

// 追加一个 MQTT 字符串（2 字节大端长度前缀 + 内容）
void AppendString(std::vector<uint8_t>& buf, const std::string& s)
{
    uint16_t n = (uint16_t)s.size();
    buf.push_back((uint8_t)(n >> 8));
    buf.push_back((uint8_t)(n & 0xFF));
    buf.insert(buf.end(), s.begin(), s.end());
}

// 阻塞地把 buf 全部写出；失败返回 false
bool SendAll(int fd, const uint8_t* buf, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::send(fd, buf + sent, len - sent, MSG_NOSIGNAL);
        if (n > 0) { sent += (size_t)n; continue; }
        if (n < 0 && (errno == EINTR)) continue;
        return false;   // 出错或对端关闭
    }
    return true;
}

// 带超时的 connect：非阻塞发起 + select 等可写 + 校验 SO_ERROR
bool ConnectWithTimeout(int fd, const struct sockaddr* addr, socklen_t alen, int timeoutMs)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return false;

    int r = ::connect(fd, addr, alen);
    if (r == 0) {
        fcntl(fd, F_SETFL, flags);   // 立即连上，恢复阻塞
        return true;
    }
    if (errno != EINPROGRESS) return false;

    fd_set wset;
    FD_ZERO(&wset);
    FD_SET(fd, &wset);
    struct timeval tv;
    tv.tv_sec  = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    r = ::select(fd + 1, nullptr, &wset, nullptr, &tv);
    if (r <= 0) return false;        // 超时或 select 出错

    int soErr = 0;
    socklen_t sl = sizeof(soErr);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soErr, &sl) < 0 || soErr != 0) return false;

    fcntl(fd, F_SETFL, flags);       // 恢复阻塞，后续 send/recv 走 SO_*TIMEO
    return true;
}

} // namespace

namespace MqttClient {

bool PublishOnce(const std::string& host, int port,
                 const std::string& clientId,
                 const std::string& topic,
                 const std::string& payload,
                 bool retain,
                 int keepAliveSec,
                 int timeoutMs)
{
    if (host.empty() || topic.empty()) {
        CLOG_ERR("mqtt: empty host/topic\n");
        return false;
    }

    // ---- 解析地址（域名 / IPv4 / IPv6 通吃）----
    char portStr[8];
    std::snprintf(portStr, sizeof(portStr), "%d", port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* res = nullptr;
    int gai = getaddrinfo(host.c_str(), portStr, &hints, &res);
    if (gai != 0 || !res) {
        CLOG_ERR("mqtt: getaddrinfo(%s:%d) failed: %s\n",
                 host.c_str(), port, gai_strerror(gai));
        return false;
    }

    // ---- 逐个候选地址尝试连接 ----
    int fd = -1;
    for (struct addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
        fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;

        // 收发超时，避免半开连接卡死后台线程
        struct timeval tv;
        tv.tv_sec  = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        if (ConnectWithTimeout(fd, ai->ai_addr, ai->ai_addrlen, timeoutMs)) break;

        ::close(fd);
        fd = -1;
    }
    freeaddrinfo(res);

    if (fd < 0) {
        CLOG_ERR("mqtt: connect %s:%d failed\n", host.c_str(), port);
        return false;
    }

    bool ok = false;
    do {
        // ---- CONNECT ----
        // 可变头：protocol name "MQTT" + level(4) + connect flags + keepalive
        std::vector<uint8_t> vh;
        AppendString(vh, "MQTT");
        vh.push_back(0x04);                       // protocol level 3.1.1
        vh.push_back(0x02);                       // flags: clean session
        vh.push_back((uint8_t)(keepAliveSec >> 8));
        vh.push_back((uint8_t)(keepAliveSec & 0xFF));
        // payload：client id
        std::vector<uint8_t> pl;
        AppendString(pl, clientId);

        std::vector<uint8_t> pkt;
        pkt.push_back(kConnect);
        EncodeRemainingLength(pkt, (uint32_t)(vh.size() + pl.size()));
        pkt.insert(pkt.end(), vh.begin(), vh.end());
        pkt.insert(pkt.end(), pl.begin(), pl.end());
        if (!SendAll(fd, pkt.data(), pkt.size())) {
            CLOG_ERR("mqtt: send CONNECT failed\n");
            break;
        }

        // ---- 等 CONNACK：期望 0x20 0x02 0x?? 0x00 ----
        uint8_t ack[4] = {0};
        ssize_t got = ::recv(fd, ack, sizeof(ack), 0);
        if (got < 4 || (ack[0] & 0xF0) != kConnack) {
            CLOG_ERR("mqtt: bad CONNACK (got=%zd, b0=0x%02x)\n", got, got > 0 ? ack[0] : 0);
            break;
        }
        if (ack[3] != 0x00) {
            CLOG_ERR("mqtt: CONNACK return code=%d (rejected)\n", ack[3]);
            break;
        }

        // ---- PUBLISH（QoS0：无 packet id）----
        std::vector<uint8_t> pubVh;
        AppendString(pubVh, topic);               // 可变头只有 topic
        std::vector<uint8_t> pub;
        // 固定头：0x30 | (retain?0x01:0)；QoS0 / DUP=0
        pub.push_back(retain ? (uint8_t)(kPublish | 0x01) : kPublish);
        EncodeRemainingLength(pub, (uint32_t)(pubVh.size() + payload.size()));
        pub.insert(pub.end(), pubVh.begin(), pubVh.end());
        pub.insert(pub.end(), payload.begin(), payload.end());
        if (!SendAll(fd, pub.data(), pub.size())) {
            CLOG_ERR("mqtt: send PUBLISH failed\n");
            break;
        }

        // ---- DISCONNECT ----
        uint8_t dis[2] = { kDisconnect, 0x00 };
        SendAll(fd, dis, sizeof(dis));            // 失败无所谓，消息已发出

        ok = true;
    } while (0);

    ::close(fd);
    return ok;
}

} // namespace MqttClient
