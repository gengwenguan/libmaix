#pragma once
#include <iostream>
#include <thread>
#include <mutex>
#include <atomic>
#include <set>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <functional>

class C_TlsContext;
class C_SslConn;

class C_HttpServer
{
public:
    // API 处理结果。业务侧只关心填好这三项，httpServer 负责拼 HTTP 响应
    //
    // 两种返回模式：
    //   A) 普通模式：填 body（短响应、JSON、文本）
    //   B) 流式文件模式：填 filePath，body 留空。这样 httpServer 会用
    //      边读边发的方式吐文件出去（64KB 缓冲），不把内容拷进堆 std::string。
    //      并且会自动处理请求里的 Range: 头返回 206 Partial Content。
    //      这是为了在 64MB 内存板子上稳定传送几百 KB ~ 几十 MB 的录像/图片，
    //      避免 ApiResponse::body 把整文件读进内存触发 OOM-killer。
    struct ApiResponse {
        int         status      = 200;          // HTTP 状态码
        std::string contentType = "application/json; charset=utf-8";
        std::string body;
        std::string filePath;  // 非空表示走流式文件模式，body 必须为空
    };

    // API 请求上下文。当前请求需要的最少必要字段：method、path、query、body
    // 后续如果要拿 header，可在这里继续加字段，无需改 httpServer
    struct ApiRequest {
        std::string method;
        std::string path;     // 已剥掉 query 字符串
        std::string query;    // 原始 query 字符串（"?" 之后，不含问号）
        std::string body;
    };

    using ApiHandler = std::function<ApiResponse(const ApiRequest&)>;

public:
    C_HttpServer(int port);
    ~C_HttpServer();

    // 可选：启用 HTTPS 端口。pTls 必须在 Start() 前调用且其生命周期长于本对象。
    // 调用后 Start() 会同时监听 m_port (HTTP) 和 tlsPort (HTTPS)。
    void EnableTls(int tlsPort, C_TlsContext* pTls);

    // 配置浏览器管理面的登录凭据和会话 token。必须在 Start() 前调用。
    void ConfigureWebAuth(const std::string& username,
                          const std::string& password,
                          const std::string& token);

    int Start();
    void Stop();

    int GetClientCount();

    // 业务侧注册 API：method 形如 "GET" / "POST"，path 形如 "/api/record/start"
    // 路由优先级高于静态文件；同 (method,path) 后注册者覆盖前者
    void RegisterApi(const std::string& method,
                     const std::string& path,
                     ApiHandler handler);

    // 注册"前缀匹配"路由（exact 路由不命中时再尝试前缀匹配）。
    // 例如 prefix = "/video/" 可以处理 "/video/xxx.mp4"。
    // handler 内通过 ApiRequest::path 拿到完整请求路径自行解析。
    void RegisterApiPrefix(const std::string& method,
                           const std::string& prefix,
                           ApiHandler handler);

private:
    void AcceptThread();
    void ProcessClient(int fd, std::shared_ptr<C_SslConn> ssl);
    void ReapClientThreads();
    void JoinClientThreads();
    void HandleHttpRequest(int fd, C_SslConn* ssl, const std::string& request);
    void SendHttpResponse(int fd, C_SslConn* ssl,
                          int statusCode, const std::string& statusText, 
                          const std::string& contentType, const std::string& content,
                          const std::string& extraHeaders = std::string());

    // 内部 IO 包装：根据 ssl 是否非空走 SSL_read/SSL_write 或 plain recv/send。
    // 非阻塞：读到 want_more / EAGAIN 时返回 -1 且 wantMore=true
    int IoRead (int fd, C_SslConn* ssl, void* buf, int len, bool& wantMore);
    int IoWrite(int fd, C_SslConn* ssl, const void* buf, int len, bool& wantMore);

    // 阻塞式把 [buf, buf+len) 全部写到 fd/ssl，遇到 EAGAIN 时 select 等待。
    // 成功返回 true；对端断开/超时返回 false。
    bool WriteAllBlocking(int fd, C_SslConn* ssl, const char* buf, size_t len);

    // 流式发送本地文件：边读盘边发送，常驻内存只用一个 64KB 栈缓冲，
    // 与文件大小完全解耦。支持 HTTP Range 单段请求（多段 Range 不支持，
    // 浏览器 video 元素几乎只用单段，足以覆盖回放场景）。
    //   rangeHeader: 来自请求里的 "Range:" 头（不含 "Range:" 前缀）；空表示无
    // 不会把文件读进 std::string，因此 64MB 内存板子也能稳定吐 GB 级文件。
    void SendFileResponse(int fd, C_SslConn* ssl,
                          const std::string& fullPath,
                          const std::string& contentType,
                          const std::string& rangeHeader);

    // 在 m_apiHandlers 里查找匹配的 handler；找到返回 true 并填 out
    bool TryDispatchApi(const std::string& method,
                        const std::string& path,
                        const std::string& query,
                        const std::string& body,
                        ApiResponse& out);

private:
    int m_port;
    int m_server_fd;
    std::atomic<bool> m_bRunFlag{false};
    std::string m_webRoot;
    std::thread m_acceptThread;
    std::mutex m_clientsMutex;
    std::set<int> m_clientFds;

    // ---- HTTPS（可选）----
    int           m_tlsPort   = 0;
    int           m_tlsServerFd = -1;
    C_TlsContext* m_pTls      = nullptr;

    // 浏览器管理面鉴权。设备到 Camera-hub 的上传链路不经过 HTTP/WS 服务。
    std::string m_authUsername;
    std::string m_authPassword;
    std::string m_authToken;

    // API 路由表：key = "METHOD path"，例如 "POST /api/record/start"
    std::mutex                          m_apiMutex;
    std::map<std::string, ApiHandler>   m_apiHandlers;
    // 前缀路由：保留注册顺序；元素 first = "METHOD prefix"
    std::vector<std::pair<std::string, ApiHandler>> m_apiPrefixHandlers;

    // ---- 并发上限（保命用，防止浏览器瞬时爆开几十个连接把 64MB 板子挤爆）----
    // 经验：单个 HTTPS 连接 ≈ TLS ctx + 64KB 读缓冲 + 80KB 线程栈，约 200KB；
    //   板子 MemAvailable 经常只剩 5~8MB，超过 6 个并发就极易触发 bad_alloc。
    // 超过上限的新连接直接 close，浏览器侧会自动排队重试，不影响功能。
    std::atomic<int>                    m_activeClients{0};
    int                                 m_maxConcurrent = 6;

    struct ClientThread {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };
    // accept 线程负责添加/回收；Stop 在 join accept 后接管并全部 join。
    std::vector<ClientThread> m_clientThreads;
};
