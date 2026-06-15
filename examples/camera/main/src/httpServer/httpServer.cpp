/*********************************************************************************
  *Copyright(C),Your Company
  *FileName:  httpServer.cpp
  *Author:    gengwenguan
  *Date:      2024-12-01
  *Description:  极简 HTTP 静态文件服务（默认根目录：<exe_dir>/web/）
  *
  *  设计要点：
  *    1. 二进制启动时通过 /proc/self/exe 定位自身所在目录，
  *       约定 web 资源放在 <exe_dir>/web/，避免把 html 嵌进 .cpp 字符串里。
  *    2. GET / 与 GET /index.html → web/index.html
  *    3. 其它 GET 请求按文件名直接读盘，按扩展名推断 Content-Type；
  *       严格拒绝包含 ".." 或绝对路径的请求，避免目录穿越。
  *    4. 文件不存在时返回 404 文本，不再 fallback 到内嵌 html，
  *       避免出现"两份漂移"的维护陷阱。
**********************************************************************************/
#include "httpServer.h"
#include "tlsContext.h"
#include "logAdapt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <errno.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <chrono>
#include <fstream>
#include <sstream>

namespace {

// 取可执行文件所在目录。失败时返回 "."（当前工作目录）
std::string GetExeDir()
{
    char buf[1024] = {0};
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) {
        CLOG_ERR("readlink /proc/self/exe failed: %s\n", strerror(errno));
        return ".";
    }
    buf[n] = '\0';
    std::string p(buf);
    size_t slash = p.find_last_of('/');
    if (slash == std::string::npos) {
        return ".";
    }
    return p.substr(0, slash);
}

// 按扩展名推断 MIME。未知后缀统一按 application/octet-stream 处理
std::string GetMimeType(const std::string& path)
{
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) {
        return "application/octet-stream";
    }
    std::string ext = path.substr(dot + 1);
    for (auto& c : ext) c = (char)tolower(c);
    if (ext == "html" || ext == "htm") return "text/html; charset=utf-8";
    if (ext == "js")                  return "application/javascript; charset=utf-8";
    if (ext == "css")                 return "text/css; charset=utf-8";
    if (ext == "json")                return "application/json; charset=utf-8";
    if (ext == "png")                 return "image/png";
    if (ext == "jpg" || ext == "jpeg")return "image/jpeg";
    if (ext == "gif")                 return "image/gif";
    if (ext == "svg")                 return "image/svg+xml";
    if (ext == "ico")                 return "image/x-icon";
    if (ext == "woff")                return "font/woff";
    if (ext == "woff2")               return "font/woff2";
    if (ext == "txt")                 return "text/plain; charset=utf-8";
    if (ext == "mp4")                 return "video/mp4";
    return "application/octet-stream";
}

// 注：原本这里有一个把整文件读进 std::string 的 ReadFileAll 工具，
// 在 64MB 内存板子上，回放页面来回点击时会触发 OOM-killer 把进程干掉。
// 现已改走 SendFileResponse 流式发送（64KB 缓冲循环 read + send），
// 与文件大小完全解耦，因此本文件不再需要 ReadFileAll。

// 校验 path：以 '/' 起始（HTTP 路径），不能含 ".."；返回去掉前导 '/' 的相对路径
// 不合法时返回空 string
std::string SanitizeUrlPath(const std::string& urlPath)
{
    if (urlPath.empty() || urlPath[0] != '/') {
        return std::string();
    }
    // 去 query / fragment（防御一下，虽然上层已经分过）
    std::string p = urlPath;
    size_t q = p.find_first_of("?#");
    if (q != std::string::npos) p.resize(q);

    // 拒绝目录穿越
    if (p.find("..") != std::string::npos) {
        return std::string();
    }
    // 拒绝绝对路径转义（"//" 开头之类）
    if (p.size() >= 2 && p[1] == '/') {
        return std::string();
    }
    if (p == "/") {
        return std::string("index.html");
    }
    return p.substr(1); // 去掉前导 '/'
}

} // namespace

C_HttpServer::C_HttpServer(int port)
    : m_port(port)
    , m_server_fd(-1)
    , m_bRunFlag(true)
{
    // 默认 web 根：<exe_dir>/web
    m_webRoot = GetExeDir() + "/web";
    CLOG_INF("HTTP web root: %s\n", m_webRoot.c_str());
}

C_HttpServer::~C_HttpServer()
{
    Stop();
}

int C_HttpServer::Start()
{
    // ---------------- HTTP 监听 ----------------
    // 用 AF_INET6 双栈：关闭 IPV6_V6ONLY 后，单个 socket 同时接收 IPv6 与 IPv4
    // 连接（IPv4 客户端以 IPv4-mapped 地址 ::ffff:a.b.c.d 形式进来）。
    // 这样局域网 IPv4 访问照常，板子的公网 IPv6 地址也能被外网直连。
    m_server_fd = socket(AF_INET6, SOCK_STREAM, 0);
    if (m_server_fd < 0) {
        CLOG_ERR("HTTP socket creation failed: %s\n", strerror(errno));
        return -1;
    }

    // Set address reuse
    int opt = 1;
    if (setsockopt(m_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        CLOG_ERR("HTTP setsockopt failed: %s\n", strerror(errno));
        close(m_server_fd);
        m_server_fd = -1;
        return -1;
    }
    // 关闭 v6only，让一个 v6 socket 同时收 v4+v6（系统默认 bindv6only=0，这里显式确保）
    int v6only = 0;
    setsockopt(m_server_fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));

    // Set non-blocking mode
    int flags = fcntl(m_server_fd, F_GETFL, 0);
    fcntl(m_server_fd, F_SETFL, flags | O_NONBLOCK);

    // Bind address
    struct sockaddr_in6 address;
    memset(&address, 0, sizeof(address));
    address.sin6_family = AF_INET6;
    address.sin6_addr   = in6addr_any;
    address.sin6_port   = htons(m_port);

    if (bind(m_server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        CLOG_ERR("HTTP bind failed: %s\n", strerror(errno));
        close(m_server_fd);
        m_server_fd = -1;
        return -1;
    }

    // Listen
    if (listen(m_server_fd, 10) < 0) {
        CLOG_ERR("HTTP listen failed: %s\n", strerror(errno));
        close(m_server_fd);
        m_server_fd = -1;
        return -1;
    }

    CLOG_INF("HTTP server started on port %d\n", m_port);
    CLOG_INF("Access URL: http://<device-ip>:%d\n", m_port);

    // ---------------- HTTPS 监听（可选） ----------------
    if (m_pTls && m_tlsPort > 0) {
        m_tlsServerFd = socket(AF_INET6, SOCK_STREAM, 0);
        if (m_tlsServerFd < 0) {
            CLOG_ERR("HTTPS socket creation failed: %s\n", strerror(errno));
        } else {
            int o = 1;
            setsockopt(m_tlsServerFd, SOL_SOCKET, SO_REUSEADDR, &o, sizeof(o));
            int v6o = 0;
            setsockopt(m_tlsServerFd, IPPROTO_IPV6, IPV6_V6ONLY, &v6o, sizeof(v6o));
            int fl = fcntl(m_tlsServerFd, F_GETFL, 0);
            fcntl(m_tlsServerFd, F_SETFL, fl | O_NONBLOCK);
            struct sockaddr_in6 addr2;
            memset(&addr2, 0, sizeof(addr2));
            addr2.sin6_family = AF_INET6;
            addr2.sin6_addr   = in6addr_any;
            addr2.sin6_port   = htons(m_tlsPort);
            if (bind(m_tlsServerFd, (struct sockaddr*)&addr2, sizeof(addr2)) < 0 ||
                listen(m_tlsServerFd, 10) < 0) {
                CLOG_ERR("HTTPS bind/listen failed: %s\n", strerror(errno));
                close(m_tlsServerFd);
                m_tlsServerFd = -1;
            } else {
                CLOG_INF("HTTPS server started on port %d\n", m_tlsPort);
                CLOG_INF("Access URL: https://<device-ip>:%d\n", m_tlsPort);
            }
        }
    }

    // Start accept thread
    m_acceptThread = std::thread(&C_HttpServer::AcceptThread, this);

    return 0;
}

void C_HttpServer::EnableTls(int tlsPort, C_TlsContext* pTls)
{
    m_tlsPort = tlsPort;
    m_pTls    = pTls;
}

void C_HttpServer::Stop()
{
    m_bRunFlag = false;

    if (m_acceptThread.joinable()) {
        m_acceptThread.join();
    }

    // Close all client connections
    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        for (int fd : m_clientFds) {
            close(fd);
        }
        m_clientFds.clear();
    }

    if (m_server_fd >= 0) {
        close(m_server_fd);
        m_server_fd = -1;
    }
    if (m_tlsServerFd >= 0) {
        close(m_tlsServerFd);
        m_tlsServerFd = -1;
    }

    CLOG_INF("HTTP server stopped\n");
}

void C_HttpServer::AcceptThread()
{
    fd_set read_fds;
    struct timeval tv;

    while (m_bRunFlag) {
        FD_ZERO(&read_fds);
        FD_SET(m_server_fd, &read_fds);
        int max_fd = m_server_fd;
        if (m_tlsServerFd >= 0) {
            FD_SET(m_tlsServerFd, &read_fds);
            if (m_tlsServerFd > max_fd) max_fd = m_tlsServerFd;
        }
        // 注：客户端 fd 不再加入 select —— 客户端处理走 detached thread，
        //   原代码这里加 client fd 没有意义（select 命中也不消费），保持简化。

        tv.tv_sec = 0;
        tv.tv_usec = 50000; // 50ms timeout

        int ret = select(max_fd + 1, &read_fds, NULL, NULL, &tv);
        if (ret < 0) {
            if (errno != EINTR) {
                CLOG_ERR("HTTP select error: %s\n", strerror(errno));
            }
            continue;
        } else if (ret == 0) {
            continue;
        }

        // 处理新连接：HTTP/HTTPS 共用同一个 ProcessClient，差别仅在握手阶段
        auto handleAccept = [this](int listenFd, bool isTls) {
            struct sockaddr_storage client_addr;   // 兼容 IPv4/IPv6 客户端地址
            socklen_t addr_len = sizeof(client_addr);
            int new_fd = accept(listenFd, (struct sockaddr *)&client_addr, &addr_len);
            if (new_fd < 0) return;

            // ---- 并发上限保命：超出 m_maxConcurrent 直接拒绝，避免 detach 出大量线程
            // 把内存撑爆触发 std::bad_alloc → terminate（这是之前观察到的崩溃模式）
            if (m_activeClients.load(std::memory_order_relaxed) >= m_maxConcurrent) {
                CLOG_ERR("HTTP%s reject new fd=%d: active=%d >= max=%d\n",
                         isTls ? "S" : "", new_fd,
                         m_activeClients.load(), m_maxConcurrent);
                // HTTP 明文场景给个 503，让浏览器知道排队；TLS 还没握手就直接 close
                if (!isTls) {
                    const char* msg =
                        "HTTP/1.0 503 Service Unavailable\r\n"
                        "Content-Length: 0\r\n"
                        "Connection: close\r\n\r\n";
                    send(new_fd, msg, (int)strlen(msg), MSG_NOSIGNAL);
                }
                close(new_fd);
                return;
            }

            // Set non-blocking
            int flags = fcntl(new_fd, F_GETFL, 0);
            fcntl(new_fd, F_SETFL, flags | O_NONBLOCK);

            std::shared_ptr<C_SslConn> sslConn;
            if (isTls) {
                // TLS 握手是阻塞-轮询的（最多 5s）；放在 accept 线程里会拖慢新连接接入，
                // 但 HTTP 服务连接频率不高，暂时简单做。后续如需可移到 client thread 内。
                auto u = m_pTls->AcceptOnFd(new_fd);
                if (!u) {
                    close(new_fd);
                    return;
                }
                sslConn = std::shared_ptr<C_SslConn>(u.release());
            }

            {
                std::lock_guard<std::mutex> lock(m_clientsMutex);
                m_clientFds.insert(new_fd);
            }
            // 计数 +1，ProcessClient 退出时 -1（在那里用 RAII 释放）
            m_activeClients.fetch_add(1, std::memory_order_relaxed);
            CLOG_INF("HTTP%s new client connected: fd=%d (active=%d)\n",
                     isTls ? "S" : "", new_fd, m_activeClients.load());

            std::thread clientThread(&C_HttpServer::ProcessClient, this, new_fd, sslConn);
            clientThread.detach();
        };

        if (FD_ISSET(m_server_fd, &read_fds)) {
            handleAccept(m_server_fd, false);
        }
        if (m_tlsServerFd >= 0 && FD_ISSET(m_tlsServerFd, &read_fds)) {
            handleAccept(m_tlsServerFd, true);
        }
    }
}

int C_HttpServer::IoRead(int fd, C_SslConn* ssl, void* buf, int len, bool& wantMore)
{
    wantMore = false;
    if (ssl) {
        return ssl->Read(buf, len, wantMore);
    }
    ssize_t n = recv(fd, buf, len, 0);
    if (n >= 0) return (int)n;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        wantMore = true;
        return -1;
    }
    return -1;
}

int C_HttpServer::IoWrite(int fd, C_SslConn* ssl, const void* buf, int len, bool& wantMore)
{
    wantMore = false;
    if (ssl) {
        return ssl->Write(buf, len, wantMore);
    }
    ssize_t n = send(fd, buf, len, MSG_NOSIGNAL);
    if (n >= 0) return (int)n;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        wantMore = true;
        return -1;
    }
    return -1;
}

void C_HttpServer::ProcessClient(int fd, std::shared_ptr<C_SslConn> ssl)
{
    // RAII：无论从哪条路径退出，都会把活跃连接计数 -1
    struct ActiveGuard {
        std::atomic<int>* p;
        ~ActiveGuard() { if (p) p->fetch_sub(1, std::memory_order_relaxed); }
    } _g{ &m_activeClients };

    std::vector<char> buffer(4096);
    std::string request;

    // Body 完整性判定：HTTP 头到达后还需根据 Content-Length 继续读，
    // 否则 POST /api/photo/delete 这种 body 几 KB 的请求会因 TCP 分段
    // 在第一段就被 dispatch，造成 JSON parser 看到截断的 names 数组而
    // 解析为空，返回 deleted=0（前端表现为"删除中..."后无任何变化）。
    size_t headEnd     = std::string::npos;
    size_t contentLen  = 0;
    bool   haveCL      = false;

    auto parseHeaderOnce = [&]() {
        if (headEnd != std::string::npos) return;
        headEnd = request.find("\r\n\r\n");
        if (headEnd == std::string::npos) return;

        // 不区分大小写查找 Content-Length:
        const std::string head = request.substr(0, headEnd);
        const std::string keyLow = "content-length:";
        std::string headLow(head.size(), ' ');
        for (size_t i = 0; i < head.size(); ++i) headLow[i] = (char)tolower((unsigned char)head[i]);

        size_t kp = headLow.find(keyLow);
        if (kp != std::string::npos) {
            size_t vp = kp + keyLow.size();
            // 跳过空白
            while (vp < head.size() && (head[vp] == ' ' || head[vp] == '\t')) ++vp;
            size_t le = head.find("\r\n", vp);
            if (le == std::string::npos) le = head.size();
            try {
                contentLen = (size_t)std::stoul(head.substr(vp, le - vp));
                haveCL = true;
            } catch (...) {
                contentLen = 0;
            }
        }
    };

    while (m_bRunFlag) {
        bool wantMore = false;
        int n = IoRead(fd, ssl.get(), buffer.data(), (int)buffer.size(), wantMore);

        if (n < 0) {
            if (wantMore) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            CLOG_ERR("HTTP recv error: %s\n", strerror(errno));
            break;
        } else if (n == 0) {
            // Client disconnected
            CLOG_INF("HTTP client disconnected: fd=%d\n", fd);
            break;
        }

        request.append(buffer.data(), n);

        parseHeaderOnce();
        if (headEnd == std::string::npos) {
            // 请求头还没收完，继续读
            continue;
        }

        // 头已就绪：根据 Content-Length 判定 body 是否够长；
        // 若没有 Content-Length（GET 等），头到即视为完整。
        size_t needTotal = headEnd + 4 + (haveCL ? contentLen : 0);
        if (request.size() < needTotal) {
            continue;
        }

        // 完整请求到达
        HandleHttpRequest(fd, ssl.get(), request);
        break; // Close connection after handling request (HTTP 1.0)
    }

    // 主动 SSL_shutdown，避免对端报 connection reset
    if (ssl) ssl->Shutdown();

    // Close connection
    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        m_clientFds.erase(fd);
    }
    close(fd);
}

void C_HttpServer::HandleHttpRequest(int fd, C_SslConn* ssl, const std::string& request)
{
    // Parse request line
    size_t lineEnd = request.find("\r\n");
    if (lineEnd == std::string::npos) {
        SendHttpResponse(fd, ssl, 400, "Bad Request", "text/plain", "Invalid HTTP request");
        return;
    }

    std::string requestLine = request.substr(0, lineEnd);

    // Parse method and path
    size_t space1 = requestLine.find(' ');
    size_t space2 = requestLine.find(' ', space1 + 1);

    if (space1 == std::string::npos || space2 == std::string::npos) {
        SendHttpResponse(fd, ssl, 400, "Bad Request", "text/plain", "Invalid HTTP request");
        return;
    }

    std::string method = requestLine.substr(0, space1);
    std::string rawPath= requestLine.substr(space1 + 1, space2 - space1 - 1);

    // 剥离 query：path?query —— path 用于路由匹配，query 透传给 handler
    std::string path  = rawPath;
    std::string query;
    {
        size_t q = path.find('?');
        if (q != std::string::npos) {
            query = path.substr(q + 1);
            path.resize(q);
        }
    }

    CLOG_INF("HTTP request: %s %s\n", method.c_str(), rawPath.c_str());

    // 提取 body（"\r\n\r\n" 之后的内容）。GET 通常无 body，POST 用得到
    std::string body;
    size_t headEnd = request.find("\r\n\r\n");
    if (headEnd != std::string::npos) {
        body = request.substr(headEnd + 4);
    }

    // 提取 Range 头（不区分大小写），API 流式文件分支和静态文件分支都需要。
    // 浏览器 <video> 拖动 + 回放页"来回点击"都会带 Range；返回 206 只发
    // 请求的一段，避免每次几百 KB 全量下载。
    std::string rangeHeader;
    {
        size_t hdrLimit = (headEnd != std::string::npos) ? headEnd : request.size();
        const char* hay = request.c_str();
        for (size_t i = 0; i + 7 < hdrLimit; ++i) {
            if (hay[i] == '\r' && hay[i+1] == '\n' &&
                (hay[i+2]=='R'||hay[i+2]=='r') &&
                (hay[i+3]=='A'||hay[i+3]=='a') &&
                (hay[i+4]=='N'||hay[i+4]=='n') &&
                (hay[i+5]=='G'||hay[i+5]=='g') &&
                (hay[i+6]=='E'||hay[i+6]=='e') &&
                hay[i+7]==':')
            {
                size_t s = i + 8;
                while (s < hdrLimit && (hay[s]==' '||hay[s]=='\t')) ++s;
                size_t e = s;
                while (e < hdrLimit && hay[e] != '\r') ++e;
                rangeHeader.assign(hay + s, hay + e);
                break;
            }
        }
    }

    // 优先尝试 API 路由（method + path 精确匹配）
    {
        ApiResponse apiResp;
        if (TryDispatchApi(method, path, query, body, apiResp)) {
            // API 可以选择"流式文件"模式：填 filePath 即可，httpServer
            // 用 64KB 缓冲流式吐出去，避免把整文件 ReadAll 进 std::string。
            // /record/*.mp4 和 /photo/*.jpg 走的就是这条路径，是上次回放
            // 页面来回点击触发 OOM 的根因修复。
            if (!apiResp.filePath.empty()) {
                SendFileResponse(fd, ssl, apiResp.filePath, apiResp.contentType, rangeHeader);
            } else {
                SendHttpResponse(fd, ssl, apiResp.status,
                                 apiResp.status == 200 ? "OK" : "ERROR",
                                 apiResp.contentType, apiResp.body);
            }
            return;
        }
    }

    if (method != "GET") {
        SendHttpResponse(fd, ssl, 405, "Method Not Allowed", "text/plain", "Method Not Allowed");
        return;
    }

    // 路径合法性校验 + 归一化
    std::string rel = SanitizeUrlPath(path);
    if (rel.empty()) {
        SendHttpResponse(fd, ssl, 400, "Bad Request", "text/plain", "Invalid path");
        return;
    }

    // 拼接 webRoot 后读盘
    std::string fullPath = m_webRoot + "/" + rel;

    // 拒绝读到目录上去
    struct stat st{};
    if (stat(fullPath.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        CLOG_INF("HTTP 404: %s (full=%s)\n", path.c_str(), fullPath.c_str());
        SendHttpResponse(fd, ssl, 404, "Not Found", "text/plain", "404 Not Found");
        return;
    }

    // 流式发送：边读边发，常驻只占 64KB；mp4 几百 MB 也不会爆内存。
    SendFileResponse(fd, ssl, fullPath, GetMimeType(rel), rangeHeader);
}

void C_HttpServer::RegisterApi(const std::string& method,
                               const std::string& path,
                               ApiHandler handler)
{
    if (!handler) return;
    std::lock_guard<std::mutex> lk(m_apiMutex);
    std::string key = method + " " + path;
    m_apiHandlers[key] = std::move(handler);
    CLOG_INF("HTTP API registered: %s\n", key.c_str());
}

void C_HttpServer::RegisterApiPrefix(const std::string& method,
                                     const std::string& prefix,
                                     ApiHandler handler)
{
    if (!handler) return;
    std::lock_guard<std::mutex> lk(m_apiMutex);
    m_apiPrefixHandlers.emplace_back(method + " " + prefix, std::move(handler));
    CLOG_INF("HTTP API prefix registered: %s%s\n", method.c_str(), (" " + prefix).c_str());
}

bool C_HttpServer::TryDispatchApi(const std::string& method,
                                  const std::string& path,
                                  const std::string& query,
                                  const std::string& body,
                                  ApiResponse& out)
{
    // path 在调用方已经剥过 query，这里直接用
    const std::string& p = path;

    ApiHandler h;
    {
        std::lock_guard<std::mutex> lk(m_apiMutex);
        auto it = m_apiHandlers.find(method + " " + p);
        if (it != m_apiHandlers.end()) {
            h = it->second;
        } else {
            // 再扫前缀路由
            std::string mp = method + " ";
            for (auto& kv : m_apiPrefixHandlers) {
                const std::string& key = kv.first; // "METHOD prefix"
                if (key.size() <= mp.size())              continue;
                if (key.compare(0, mp.size(), mp) != 0)   continue;
                std::string pfx = key.substr(mp.size());
                if (p.size() >= pfx.size() &&
                    p.compare(0, pfx.size(), pfx) == 0) {
                    h = kv.second;
                    break;
                }
            }
            if (!h) return false;
        }
    }

    ApiRequest req;
    req.method = method;
    req.path   = p;
    req.query  = query;
    req.body   = body;
    try {
        out = h(req);
    } catch (const std::exception& e) {
        CLOG_ERR("HTTP API handler exception: %s\n", e.what());
        out.status = 500;
        out.contentType = "text/plain";
        out.body = std::string("internal error: ") + e.what();
    } catch (...) {
        out.status = 500;
        out.contentType = "text/plain";
        out.body = "internal error";
    }
    return true;
}

void C_HttpServer::SendHttpResponse(int fd, C_SslConn* ssl,
                                   int statusCode, const std::string& statusText,
                                   const std::string& contentType, const std::string& content)
{
    std::string response = "HTTP/1.1 " + std::to_string(statusCode) + " " + statusText + "\r\n";
    response += "Content-Type: " + contentType + "\r\n";
    response += "Content-Length: " + std::to_string(content.length()) + "\r\n";
    response += "Connection: close\r\n";
    response += "Cache-Control: no-cache, no-store, must-revalidate\r\n";
    response += "Pragma: no-cache\r\n";
    response += "Expires: 0\r\n";
    response += "\r\n";
    response += content;

    // 头 + 体一起阻塞写（仍然只为"短响应"使用：API JSON / 4xx 文本等）。
    // 大文件请走 SendFileResponse —— 那条路只把 64KB 缓冲塞 socket，永远不会
    // 把整文件 + response 拼在一起出现 2× 内存峰值。
    WriteAllBlocking(fd, ssl, response.data(), response.size());
}

bool C_HttpServer::WriteAllBlocking(int fd, C_SslConn* ssl, const char* buf, size_t len)
{
    // 非阻塞 socket 单次 send 可能 short write，尤其几百 KB 数据；
    // 这里循环写直到全部发完或对端断开/超时。TLS 同理（SSL_write 也可能 want_write）。
    const char* p   = buf;
    size_t      rem = len;
    while (rem > 0) {
        bool wantMore = false;
        int n = IoWrite(fd, ssl, p, (int)rem, wantMore);
        if (n > 0) {
            p   += n;
            rem -= (size_t)n;
            continue;
        }
        if (n < 0 && wantMore) {
            // 对端缓冲区暂满或 TLS 内部需要更多 IO，等可写。
            // 注意：超时不能太长，否则浏览器来回切片导致大量 fd 滞留服务端，
            // 在 64MB 板子上很容易堆积线程引爆 OOM。1s 已经足够区分"瞬时拥塞"
            // 与"对端真断连"（TCP RST 后 select 立即返回 r=1，send 才报 EPIPE）。
            fd_set wfds;
            FD_ZERO(&wfds); FD_SET(fd, &wfds);
            struct timeval tv; tv.tv_sec = 1; tv.tv_usec = 0;
            int r = select(fd + 1, NULL, &wfds, NULL, &tv);
            if (r <= 0) {
                // 静默：用户来回切片产生大量"主动 abort"是正常现象，不必每次刷一行 ERR
                return false;
            }
            continue;
        }
        CLOG_ERR("HTTP send fail: %s (rem=%zu)\n", strerror(errno), rem);
        return false;
    }
    return true;
}

// 解析 HTTP "Range:" 头，仅支持单段 "bytes=START-END"（END 可省略）。
// 返回 true 时填 outFirst/outLast；返回 false 表示语法错误或多段（应回 416）。
// 调用者拿到 [first, last] 后还要自己 clamp 到文件长度。
static bool ParseSimpleRange(const std::string& rangeHeader,
                             int64_t fileSize,
                             int64_t& outFirst,
                             int64_t& outLast)
{
    // 形如 "bytes=0-499", "bytes=500-", "bytes=-500"（最后 500 字节）
    if (rangeHeader.empty() || fileSize <= 0) return false;
    const std::string prefix = "bytes=";
    if (rangeHeader.compare(0, prefix.size(), prefix) != 0) return false;
    std::string r = rangeHeader.substr(prefix.size());
    // 多段（含逗号）暂不支持
    if (r.find(',') != std::string::npos) return false;
    size_t dash = r.find('-');
    if (dash == std::string::npos) return false;
    std::string s1 = r.substr(0, dash);
    std::string s2 = r.substr(dash + 1);
    auto trim = [](std::string& s){
        while (!s.empty() && (s.front()==' '||s.front()=='\t')) s.erase(s.begin());
        while (!s.empty() && (s.back() ==' '||s.back() =='\t')) s.pop_back();
    };
    trim(s1); trim(s2);

    if (s1.empty()) {
        // "-N" 表示最后 N 字节
        if (s2.empty()) return false;
        int64_t n = (int64_t)strtoll(s2.c_str(), nullptr, 10);
        if (n <= 0) return false;
        if (n > fileSize) n = fileSize;
        outFirst = fileSize - n;
        outLast  = fileSize - 1;
        return true;
    }
    int64_t first = (int64_t)strtoll(s1.c_str(), nullptr, 10);
    int64_t last;
    if (s2.empty()) {
        last = fileSize - 1;
    } else {
        last = (int64_t)strtoll(s2.c_str(), nullptr, 10);
    }
    if (first < 0 || last < first) return false;
    if (last >= fileSize) last = fileSize - 1;
    outFirst = first;
    outLast  = last;
    return true;
}

void C_HttpServer::SendFileResponse(int fd, C_SslConn* ssl,
                                    const std::string& fullPath,
                                    const std::string& contentType,
                                    const std::string& rangeHeader)
{
    // 1) 用 open 而不是 ifstream，避免任何把文件内容读进 string 的中间态
    int ffd = open(fullPath.c_str(), O_RDONLY | O_CLOEXEC);
    if (ffd < 0) {
        CLOG_ERR("SendFileResponse open fail: %s (%s)\n", fullPath.c_str(), strerror(errno));
        SendHttpResponse(fd, ssl, 500, "Internal Server Error", "text/plain", "open fail");
        return;
    }
    struct stat st{};
    if (fstat(ffd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(ffd);
        SendHttpResponse(fd, ssl, 404, "Not Found", "text/plain", "404 Not Found");
        return;
    }
    const int64_t fileSize = (int64_t)st.st_size;

    // 2) 解析 Range（可选）
    int64_t first = 0, last = fileSize - 1;
    bool isPartial = false;
    if (!rangeHeader.empty()) {
        if (ParseSimpleRange(rangeHeader, fileSize, first, last)) {
            isPartial = true;
        } else {
            // 语法错或不支持的多段：返回 416
            close(ffd);
            std::string body = "Requested Range Not Satisfiable";
            std::string hdr  = "HTTP/1.1 416 Requested Range Not Satisfiable\r\n";
            hdr += "Content-Type: text/plain\r\n";
            hdr += "Content-Range: bytes */" + std::to_string(fileSize) + "\r\n";
            hdr += "Content-Length: " + std::to_string(body.size()) + "\r\n";
            hdr += "Connection: close\r\n\r\n";
            hdr += body;
            WriteAllBlocking(fd, ssl, hdr.data(), hdr.size());
            return;
        }
    }
    const int64_t sendLen = last - first + 1;

    // 3) 拼响应头（不含 body），单独发送，body 走流式
    std::string hdr;
    hdr.reserve(256);
    if (isPartial) {
        hdr += "HTTP/1.1 206 Partial Content\r\n";
    } else {
        hdr += "HTTP/1.1 200 OK\r\n";
    }
    hdr += "Content-Type: " + contentType + "\r\n";
    hdr += "Accept-Ranges: bytes\r\n";
    hdr += "Content-Length: " + std::to_string(sendLen) + "\r\n";
    if (isPartial) {
        hdr += "Content-Range: bytes " + std::to_string(first) + "-" +
               std::to_string(last) + "/" + std::to_string(fileSize) + "\r\n";
    }
    hdr += "Connection: close\r\n";
    // mp4/jpeg 这类静态资源允许浏览器短缓存，避免回放页拖动反复全量下载
    hdr += "Cache-Control: public, max-age=60\r\n";
    hdr += "\r\n";

    if (!WriteAllBlocking(fd, ssl, hdr.data(), hdr.size())) {
        close(ffd);
        return;
    }

    // 4) seek + 流式 64KB 循环读发，永远不在堆上保留整文件
    if (first > 0 && lseek(ffd, (off_t)first, SEEK_SET) == (off_t)-1) {
        CLOG_ERR("SendFileResponse lseek fail: %s\n", strerror(errno));
        close(ffd);
        return;
    }
    char buf[64 * 1024];
    int64_t remain = sendLen;
    while (remain > 0) {
        size_t want = (remain > (int64_t)sizeof(buf)) ? sizeof(buf) : (size_t)remain;
        ssize_t got = read(ffd, buf, want);
        if (got <= 0) {
            if (got < 0 && (errno == EINTR)) continue;
            CLOG_ERR("SendFileResponse read fail/short: got=%zd errno=%s rem=%lld\n",
                     got, strerror(errno), (long long)remain);
            break;
        }
        if (!WriteAllBlocking(fd, ssl, buf, (size_t)got)) {
            // 对端早断（用户来回点击 → 浏览器 abort 旧请求），属正常情况
            break;
        }
        remain -= got;
    }
    close(ffd);
}

int C_HttpServer::GetClientCount()
{
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    return m_clientFds.size();
}
