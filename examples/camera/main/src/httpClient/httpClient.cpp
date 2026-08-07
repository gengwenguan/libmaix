/*********************************************************************************
  *Copyright(C),Your Company
  *FileName:  httpClient.cpp
  *Author:    gengwenguan
  *Date:      2026-07-18
  *Description:  极简出站 HTTP/1.0 POST 客户端实现。详见 httpClient.h。
**********************************************************************************/
#include "httpClient.h"
#include "logAdapt.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace {

// 解析 http://host[:port][/path]。仅支持明文 http://。
//   host 支持域名 / IPv4 / [IPv6]（方括号形式，端口在括号外）。
// 成功返回 true 并填 host/port/path；path 至少为 "/"。
bool ParseHttpUrl(const std::string& url,
                  std::string& host, int& port, std::string& path)
{
    const std::string scheme = "http://";
    if (url.size() <= scheme.size()) return false;
    // 大小写不敏感比较 scheme
    for (size_t i = 0; i < scheme.size(); ++i) {
        if (std::tolower((unsigned char)url[i]) != scheme[i]) return false;
    }

    size_t p = scheme.size();
    // authority 到第一个 '/'（或串尾）为止
    size_t slash = url.find('/', p);
    std::string authority = (slash == std::string::npos)
                              ? url.substr(p)
                              : url.substr(p, slash - p);
    path = (slash == std::string::npos) ? "/" : url.substr(slash);
    if (authority.empty()) return false;

    port = 80;
    if (authority[0] == '[') {
        // [IPv6]:port —— 端口在方括号外
        size_t rb = authority.find(']');
        if (rb == std::string::npos) return false;
        host = authority.substr(1, rb - 1);
        if (rb + 1 < authority.size()) {
            if (authority[rb + 1] != ':') return false;
            const std::string ps = authority.substr(rb + 2);
            if (ps.empty()) return false;
            for (char c : ps) if (c < '0' || c > '9') return false;
            port = std::atoi(ps.c_str());
        }
    } else {
        size_t colon = authority.rfind(':');
        if (colon == std::string::npos) {
            host = authority;
        } else {
            host = authority.substr(0, colon);
            const std::string ps = authority.substr(colon + 1);
            if (ps.empty()) return false;
            for (char c : ps) if (c < '0' || c > '9') return false;
            port = std::atoi(ps.c_str());
        }
    }
    if (host.empty() || port <= 0 || port > 65535) return false;
    return true;
}

// 阻塞地把 buf 全部写出；失败返回 false
bool SendAll(int fd, const char* buf, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::send(fd, buf + sent, len - sent, MSG_NOSIGNAL);
        if (n > 0) { sent += (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        return false;
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
    if (r == 0) { fcntl(fd, F_SETFL, flags); return true; }
    if (errno != EINPROGRESS) return false;

    fd_set wset;
    FD_ZERO(&wset);
    FD_SET(fd, &wset);
    struct timeval tv;
    tv.tv_sec  = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    r = ::select(fd + 1, nullptr, &wset, nullptr, &tv);
    if (r <= 0) return false;

    int soErr = 0;
    socklen_t sl = sizeof(soErr);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soErr, &sl) < 0 || soErr != 0) return false;

    fcntl(fd, F_SETFL, flags);
    return true;
}

// 从 HTTP 状态行 "HTTP/1.x <code> <reason>" 里抽状态码；失败返回 0
int ParseStatusCode(const char* buf, size_t len)
{
    // 至少 "HTTP/1.1 200"
    if (len < 12) return 0;
    if (std::strncmp(buf, "HTTP/", 5) != 0) return 0;
    size_t sp = 0;
    while (sp < len && buf[sp] != ' ') ++sp;      // 跳过 "HTTP/1.x"
    while (sp < len && buf[sp] == ' ') ++sp;      // 跳过空格
    if (sp + 3 > len) return 0;
    int code = 0;
    for (int i = 0; i < 3; ++i) {
        char c = buf[sp + i];
        if (c < '0' || c > '9') return 0;
        code = code * 10 + (c - '0');
    }
    return code;
}

} // namespace

namespace HttpClient {

PostResult Post(const std::string& url,
                const std::string& body,
                const std::string& contentType,
                int                timeoutMs)
{
    PostResult res;

    std::string host, path;
    int port = 80;
    if (!ParseHttpUrl(url, host, port, path)) {
        res.err = "bad url (only http:// supported)";
        CLOG_ERR("httpClient: bad url: %s\n", url.c_str());
        return res;
    }

    // ---- 解析地址（域名 / IPv4 / IPv6 通吃）----
    char portStr[8];
    std::snprintf(portStr, sizeof(portStr), "%d", port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* ai = nullptr;
    int gai = getaddrinfo(host.c_str(), portStr, &hints, &ai);
    if (gai != 0 || !ai) {
        res.err = "dns failed";
        CLOG_ERR("httpClient: getaddrinfo(%s:%d) failed: %s\n",
                 host.c_str(), port, gai_strerror(gai));
        return res;
    }

    int fd = -1;
    for (struct addrinfo* p = ai; p != nullptr; p = p->ai_next) {
        fd = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;

        struct timeval tv;
        tv.tv_sec  = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        if (ConnectWithTimeout(fd, p->ai_addr, p->ai_addrlen, timeoutMs)) break;
        ::close(fd);
        fd = -1;
    }
    freeaddrinfo(ai);

    if (fd < 0) {
        res.err = "connect failed";
        CLOG_ERR("httpClient: connect %s:%d failed\n", host.c_str(), port);
        return res;
    }

    // ---- 拼请求：HTTP/1.0 + Connection: close，避免处理 keep-alive/分块 ----
    // Host 头用原始 host（域名或 IP）；IPv6 字面量补回方括号符合规范。
    std::string hostHeader = (host.find(':') != std::string::npos)
                               ? ("[" + host + "]") : host;
    if (port != 80) { hostHeader += ":"; hostHeader += portStr; }

    std::string req;
    req.reserve(256 + body.size());
    req += "POST ";
    req += path;
    req += " HTTP/1.0\r\n";
    req += "Host: " + hostHeader + "\r\n";
    req += "User-Agent: v831cam-actionproxy\r\n";
    req += "Connection: close\r\n";
    if (!body.empty()) {
        req += "Content-Type: " + contentType + "\r\n";
    }
    req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    req += "\r\n";
    req += body;

    if (!SendAll(fd, req.data(), req.size())) {
        res.err = "send failed";
        CLOG_ERR("httpClient: send to %s failed\n", url.c_str());
        ::close(fd);
        return res;
    }

    // ---- 读响应首块，取状态行 ----
    char buf[512] = {0};
    ssize_t got = ::recv(fd, buf, sizeof(buf) - 1, 0);
    ::close(fd);

    if (got <= 0) {
        res.err = "no response";
        CLOG_ERR("httpClient: no response from %s\n", url.c_str());
        return res;
    }

    int code = ParseStatusCode(buf, (size_t)got);
    if (code == 0) {
        res.err = "bad response";
        CLOG_ERR("httpClient: bad status line from %s\n", url.c_str());
        return res;
    }

    res.ok     = true;
    res.status = code;
    CLOG_INF("httpClient: POST %s -> %d\n", url.c_str(), code);
    return res;
}

} // namespace HttpClient
