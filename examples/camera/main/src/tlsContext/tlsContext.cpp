/*********************************************************************************
  *FileName:  tlsContext.cpp
  *Description: OpenSSL 1.1.1 server side wrapper。链接到 libssl.so.1.1 + libcrypto.so.1.1。
**********************************************************************************/
#include "tlsContext.h"
#include "logAdapt.h"
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/conf.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>

namespace {
// 进程级一次性 OpenSSL 初始化。OpenSSL 1.1+ 一般会自动初始化，但为兼容老版本/确定性，
// 仍显式调用 SSL_library_init 等。多次调用是幂等的。
static void EnsureSslInited()
{
    static bool inited = false;
    if (inited) return;
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();
    inited = true;
}

static void DumpOpenSslErr(const char* tag)
{
    unsigned long e;
    while ((e = ERR_get_error()) != 0) {
        char buf[256] = {0};
        ERR_error_string_n(e, buf, sizeof(buf));
        CLOG_ERR("openssl[%s]: %s\n", tag, buf);
    }
}
} // namespace

// ---------------- C_SslConn ---------------------------------------------------

C_SslConn::C_SslConn(SSL* ssl)
    : m_ssl(ssl), m_fd(-1)
{}

C_SslConn::~C_SslConn()
{
    if (m_ssl) {
        // 不调 SSL_shutdown 也无所谓：fd 已经会被调用方 close，对端会感知到 RST/FIN
        SSL_free(m_ssl);
        m_ssl = nullptr;
    }
}

void C_SslConn::Shutdown()
{
    if (!m_ssl) return;
    // 单次 shutdown 即可；远端可能已经先关，没必要等 bidirectional shutdown
    SSL_shutdown(m_ssl);
}

int C_SslConn::Read(void* buf, int len, bool& wantMore)
{
    wantMore = false;
    if (!m_ssl) return -1;
    int n = SSL_read(m_ssl, buf, len);
    if (n > 0) return n;
    int err = SSL_get_error(m_ssl, n);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
        wantMore = true;
        return -1;
    }
    if (err == SSL_ERROR_ZERO_RETURN) {
        return 0; // 对端 close_notify
    }
    // 其它真实错误
    DumpOpenSslErr("read");
    return -1;
}

int C_SslConn::Write(const void* buf, int len, bool& wantMore)
{
    wantMore = false;
    if (!m_ssl) return -1;
    int n = SSL_write(m_ssl, buf, len);
    if (n > 0) return n;
    int err = SSL_get_error(m_ssl, n);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
        wantMore = true;
        return -1;
    }
    DumpOpenSslErr("write");
    return -1;
}

// ---------------- C_TlsContext -------------------------------------------------

C_TlsContext::C_TlsContext()
    : m_ctx(nullptr)
{
    EnsureSslInited();
}

C_TlsContext::~C_TlsContext()
{
    if (m_ctx) {
        SSL_CTX_free(m_ctx);
        m_ctx = nullptr;
    }
}

bool C_TlsContext::Init(const std::string& certPemPath, const std::string& keyPemPath)
{
    EnsureSslInited();

    // SSLv23_server_method 是历史遗留名字，实际允许 TLS1.0~TLS1.3 协商
    const SSL_METHOD* method = SSLv23_server_method();
    m_ctx = SSL_CTX_new(method);
    if (!m_ctx) {
        DumpOpenSslErr("ctx_new");
        return false;
    }

    // 禁用过时协议；至少 TLS 1.2，浏览器全都支持
    SSL_CTX_set_options(m_ctx,
        SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 | SSL_OP_NO_TLSv1 | SSL_OP_NO_TLSv1_1);

    // 证书 + 私钥
    if (SSL_CTX_use_certificate_file(m_ctx, certPemPath.c_str(), SSL_FILETYPE_PEM) <= 0) {
        DumpOpenSslErr("use_cert");
        CLOG_ERR("TLS load cert fail: %s\n", certPemPath.c_str());
        return false;
    }
    if (SSL_CTX_use_PrivateKey_file(m_ctx, keyPemPath.c_str(), SSL_FILETYPE_PEM) <= 0) {
        DumpOpenSslErr("use_key");
        CLOG_ERR("TLS load key fail: %s\n", keyPemPath.c_str());
        return false;
    }
    if (!SSL_CTX_check_private_key(m_ctx)) {
        DumpOpenSslErr("key_check");
        CLOG_ERR("TLS cert/key mismatch\n");
        return false;
    }

    CLOG_INF("TLS context inited (cert=%s, key=%s)\n",
             certPemPath.c_str(), keyPemPath.c_str());
    return true;
}

std::unique_ptr<C_SslConn> C_TlsContext::AcceptOnFd(int fd)
{
    if (!m_ctx) return nullptr;

    SSL* ssl = SSL_new(m_ctx);
    if (!ssl) {
        DumpOpenSslErr("ssl_new");
        return nullptr;
    }
    if (SSL_set_fd(ssl, fd) != 1) {
        DumpOpenSslErr("set_fd");
        SSL_free(ssl);
        return nullptr;
    }

    // 非阻塞 SSL_accept：循环直到 want_read/want_write 都不再来；最多等 5s
    auto deadline = []() {
        struct timeval tv;
        gettimeofday(&tv, nullptr);
        return (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
    };
    uint64_t t0 = deadline();
    while (true) {
        int r = SSL_accept(ssl);
        if (r == 1) break; // 握手成功
        int err = SSL_get_error(ssl, r);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
            // 等可读/可写
            fd_set rfds, wfds;
            FD_ZERO(&rfds); FD_ZERO(&wfds);
            if (err == SSL_ERROR_WANT_READ)  FD_SET(fd, &rfds);
            else                             FD_SET(fd, &wfds);
            struct timeval tv;
            tv.tv_sec = 1;
            tv.tv_usec = 0;
            int s = select(fd + 1, &rfds, &wfds, nullptr, &tv);
            if (s < 0 && errno != EINTR) {
                CLOG_ERR("TLS handshake select err: %s\n", strerror(errno));
                SSL_free(ssl);
                return nullptr;
            }
            if (deadline() - t0 > 5000) {
                CLOG_ERR("TLS handshake timeout (fd=%d)\n", fd);
                SSL_free(ssl);
                return nullptr;
            }
            continue;
        }
        // 真实失败
        DumpOpenSslErr("ssl_accept");
        CLOG_ERR("TLS handshake fail (fd=%d, err=%d)\n", fd, err);
        SSL_free(ssl);
        return nullptr;
    }

    auto conn = std::unique_ptr<C_SslConn>(new C_SslConn(ssl));
    conn->SetFd(fd);
    return conn;
}
