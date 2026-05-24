/*********************************************************************************
  *FileName:  tlsContext.h
  *Description: 封装 OpenSSL SSL_CTX + 单连接 IO，对外抽象出 SslConn 句柄。
  *             httpServer/websocketServer 在原有 fd 基础上，附加 SslConn* 字段，
  *             所有 recv/send 改走 TlsRead/TlsWrite，插入 TLS 时仅几行差别。
  *
  *  设计目标：
  *    1. .h 不暴露 openssl 头文件，避免污染所有引用方；类型用前向声明。
  *    2. 只支持服务端（accept），不做客户端用法。
  *    3. SslConn 与 fd 1:1 绑定；销毁 SslConn 时 SSL_shutdown + SSL_free，但
  *       fd 仍由调用方负责 close（保持与现有 close(fd) 流程一致）。
  *    4. 错误返回 -1，业务侧像处理 errno EAGAIN/EWOULDBLOCK 一样判 wantRead/wantWrite。
**********************************************************************************/
#pragma once
#include <string>
#include <memory>

// 前向声明，避免 .h 强制 include <openssl/ssl.h>
struct ssl_ctx_st;
struct ssl_st;
typedef struct ssl_ctx_st SSL_CTX;
typedef struct ssl_st     SSL;

class C_TlsContext;

// 单条 TLS 连接（与 socket fd 1:1）
class C_SslConn {
public:
    explicit C_SslConn(SSL* ssl);
    ~C_SslConn();

    // 关联底层 socket fd（在 SSL_set_fd 后保存以便日志）
    int  Fd() const   { return m_fd; }
    void SetFd(int f) { m_fd = f; }

    // 阻塞-非阻塞透明：返回 >0 = 实际字节数；0 = 对端关；-1 = 错误（看 wantRead/wantWrite）
    // wantMore=true 表示需要等可读/可写后重试（类似 EAGAIN）。
    int Read (void* buf, int len, bool& wantMore);
    int Write(const void* buf, int len, bool& wantMore);

    // 主动 SSL_shutdown（一般在 close 前调用）。失败也无妨，析构里仍会 SSL_free
    void Shutdown();

    SSL* Raw() { return m_ssl; }

private:
    SSL* m_ssl;
    int  m_fd;
};

// 服务端 SSL_CTX 持有者；进程内单例使用即可
class C_TlsContext {
public:
    C_TlsContext();
    ~C_TlsContext();

    // 加载 PEM 证书 + 私钥；失败返回 false 并打日志
    // 进程内只调用一次。
    bool Init(const std::string& certPemPath, const std::string& keyPemPath);

    // 给一个已 accept 的 socket fd 包一层 SSL，并完成握手（非阻塞循环 + select 1s 超时）。
    // 失败返回 nullptr；成功返回新建的 C_SslConn 所有权。
    std::unique_ptr<C_SslConn> AcceptOnFd(int fd);

    SSL_CTX* Ctx() { return m_ctx; }

private:
    SSL_CTX* m_ctx;
};
