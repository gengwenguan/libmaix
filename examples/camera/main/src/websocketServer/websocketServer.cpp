/*********************************************************************************
  *Copyright(C),Your Company
  *FileName:  websocketServer.cpp
  *Author:    gengwenguan
  *Date:      2024-12-01
  *Description:  WebSocket服务器实现（不依赖OpenSSL）
**********************************************************************************/
#include "websocketServer.h"
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
#include <netinet/tcp.h>     // TCP_NODELAY
#include <fcntl.h>
#include <chrono>
#include <fstream>
#include <dirent.h>
#include <algorithm>
#include <limits>

// WebSocket GUID (RFC 6455)
static const char WS_GUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

// 单客户端只保留少量待发直播数据。fMP4 按 IDR 切片、ADTS 每帧可独立解码，
// 队列满时丢最老数据即可追到实时位置，不允许慢客户端拖住编码线程。
static const size_t kMaxQueuedFrames = 8;
static const size_t kMaxQueuedBytes  = 1024 * 1024;
static const size_t kMaxHandshakeBytes = 16 * 1024;
static const size_t kMaxClientPayload  = 64 * 1024;
static const auto   kHandshakeTimeout  = std::chrono::seconds(5);

static int OpenDualStackListener(int port, const char* label)
{
    int fd = socket(AF_INET6, SOCK_STREAM, 0);
    if (fd < 0) {
        CLOG_ERR("%s socket创建失败: %s\n", label, strerror(errno));
        return -1;
    }

    int reuse = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        CLOG_ERR("%s setsockopt失败: %s\n", label, strerror(errno));
        close(fd);
        return -1;
    }
    int v6only = 0;
    setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        CLOG_ERR("%s nonblock设置失败: %s\n", label, strerror(errno));
        close(fd);
        return -1;
    }

    struct sockaddr_in6 address;
    memset(&address, 0, sizeof(address));
    address.sin6_family = AF_INET6;
    address.sin6_addr   = in6addr_any;
    address.sin6_port   = htons(port);
    if (bind(fd, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)) < 0 ||
        listen(fd, 10) < 0) {
        CLOG_ERR("%s bind/listen失败: %s\n", label, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

// 简单的SHA1实现
class SimpleSHA1 {
public:
    static std::string hash(const std::string& input) {
        uint32_t h0 = 0x67452301;
        uint32_t h1 = 0xEFCDAB89;
        uint32_t h2 = 0x98BADCFE;
        uint32_t h3 = 0x10325476;
        uint32_t h4 = 0xC3D2E1F0;

        uint64_t ml = input.length() * 8;
        std::string msg = input;
        msg += (char)0x80;
        while ((msg.length() % 64) != 56) {
            msg += (char)0x00;
        }
        for (int i = 7; i >= 0; i--) {
            msg += (char)((ml >> (i * 8)) & 0xFF);
        }

        for (size_t i = 0; i < msg.length(); i += 64) {
            uint32_t w[80];
            for (int j = 0; j < 16; j++) {
                w[j] = ((uint8_t)msg[i + j * 4] << 24) |
                       ((uint8_t)msg[i + j * 4 + 1] << 16) |
                       ((uint8_t)msg[i + j * 4 + 2] << 8) |
                       ((uint8_t)msg[i + j * 4 + 3]);
            }
            for (int j = 16; j < 80; j++) {
                w[j] = leftrotate(w[j - 3] ^ w[j - 8] ^ w[j - 14] ^ w[j - 16], 1);
            }

            uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;

            for (int j = 0; j < 80; j++) {
                uint32_t f, k;
                if (j < 20) {
                    f = (b & c) | ((~b) & d);
                    k = 0x5A827999;
                } else if (j < 40) {
                    f = b ^ c ^ d;
                    k = 0x6ED9EBA1;
                } else if (j < 60) {
                    f = (b & c) | (b & d) | (c & d);
                    k = 0x8F1BBCDC;
                } else {
                    f = b ^ c ^ d;
                    k = 0xCA62C1D6;
                }

                uint32_t temp = leftrotate(a, 5) + f + e + k + w[j];
                e = d;
                d = c;
                c = leftrotate(b, 30);
                b = a;
                a = temp;
            }

            h0 += a;
            h1 += b;
            h2 += c;
            h3 += d;
            h4 += e;
        }

        std::string result;
        result += (char)((h0 >> 24) & 0xFF);
        result += (char)((h0 >> 16) & 0xFF);
        result += (char)((h0 >> 8) & 0xFF);
        result += (char)(h0 & 0xFF);
        result += (char)((h1 >> 24) & 0xFF);
        result += (char)((h1 >> 16) & 0xFF);
        result += (char)((h1 >> 8) & 0xFF);
        result += (char)(h1 & 0xFF);
        result += (char)((h2 >> 24) & 0xFF);
        result += (char)((h2 >> 16) & 0xFF);
        result += (char)((h2 >> 8) & 0xFF);
        result += (char)(h2 & 0xFF);
        result += (char)((h3 >> 24) & 0xFF);
        result += (char)((h3 >> 16) & 0xFF);
        result += (char)((h3 >> 8) & 0xFF);
        result += (char)(h3 & 0xFF);
        result += (char)((h4 >> 24) & 0xFF);
        result += (char)((h4 >> 16) & 0xFF);
        result += (char)((h4 >> 8) & 0xFF);
        result += (char)(h4 & 0xFF);

        return result;
    }

private:
    static uint32_t leftrotate(uint32_t x, uint32_t c) {
        return (x << c) | (x >> (32 - c));
    }
};

// 简单的Base64实现
class SimpleBase64 {
public:
    static std::string encode(const std::string& input) {
        static const char base64_chars[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

        std::string result;
        int i = 0;
        uint8_t char_array_3[3];
        uint8_t char_array_4[4];

        for (size_t in_len = 0; in_len < input.length(); in_len++) {
            char_array_3[i++] = input[in_len];
            if (i == 3) {
                char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
                char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
                char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
                char_array_4[3] = char_array_3[2] & 0x3f;

                for (int j = 0; j < 4; j++)
                    result += base64_chars[char_array_4[j]];
                i = 0;
            }
        }

        if (i) {
            for (int j = i; j < 3; j++)
                char_array_3[j] = '\0';

            char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
            char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
            char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);

            for (int j = 0; j < (i + 1); j++)
                result += base64_chars[char_array_4[j]];

            while ((i++ < 3))
                result += '=';
        }

        return result;
    }
};

C_WebSocketServer::C_WebSocketServer(C_Listener* pListener, int port, bool isLive)
    : m_pListener(pListener)
    , m_port(port)
    , m_isLive(isLive)
    , m_server_fd(-1)
{
    // 直播允许多终端观看；旧 WS 回放仅保留兼容，限制更紧以保护 64MB 内存。
    m_maxClients = m_isLive ? 6 : 2;
}

void C_WebSocketServer::EnableTls(int tlsPort, C_TlsContext* pTls)
{
    m_tlsPort = tlsPort;
    m_pTls    = pTls;
}

int C_WebSocketServer::Start()
{
    bool expected = false;
    if (!m_bRunFlag.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return 0;
    }

    m_server_fd = OpenDualStackListener(m_port, "WebSocket");
    if (m_server_fd < 0) {
        m_bRunFlag.store(false, std::memory_order_release);
        return -1;
    }

    if (m_pTls && m_tlsPort > 0) {
        m_tlsServerFd = OpenDualStackListener(m_tlsPort, "WSS");
        if (m_tlsServerFd < 0) {
            CLOG_ERR("WSS端口 %d 启动失败，继续提供明文 WS\n", m_tlsPort);
        }
    }

    if (m_isLive) {
        C_LiveHub::Inst().Subscribe(this);
        m_liveSubscribed = true;
    }

    try {
        m_clientThreads.reserve(m_maxClients + 2);
        m_acceptThread = std::thread(&C_WebSocketServer::AcceptThread, this);
    } catch (...) {
        Stop();
        CLOG_ERR("WebSocket accept线程创建失败\n");
        return -1;
    }

    CLOG_INF("WebSocket服务器启动在端口 %d (%s)\n",
             m_port, m_isLive ? "直播" : "回放");
    if (m_tlsServerFd >= 0) {
        CLOG_INF("WSS服务器启动在端口 %d (%s)\n",
                 m_tlsPort, m_isLive ? "直播" : "回放");
    }
    return 0;
}

void C_WebSocketServer::Stop()
{
    m_bRunFlag.store(false, std::memory_order_release);

    if (m_liveSubscribed) {
        C_LiveHub::Inst().Unsubscribe(this);
        m_liveSubscribed = false;
    }

    if (m_server_fd >= 0) shutdown(m_server_fd, SHUT_RDWR);
    if (m_tlsServerFd >= 0) shutdown(m_tlsServerFd, SHUT_RDWR);

    if (m_acceptThread.joinable()) {
        m_acceptThread.join();
    }

    // 只 shutdown 唤醒工作线程；最终 close 仍由各工作线程唯一负责，避免 fd
    // 被系统复用后 Stop 与线程退出路径重复 close 到新的连接。
    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        for (int fd : m_clientFds) {
            shutdown(fd, SHUT_RDWR);
        }
    }
    JoinClientThreads();

    if (m_server_fd >= 0) {
        close(m_server_fd);
        m_server_fd = -1;
    }
    if (m_tlsServerFd >= 0) {
        close(m_tlsServerFd);
        m_tlsServerFd = -1;
    }

    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        m_clientFds.clear();
        m_clients.clear();
    }

    CLOG_INF("WebSocket服务器停止\n");
}

C_WebSocketServer::~C_WebSocketServer()
{
    Stop();
}

void C_WebSocketServer::AcceptThread()
{
    fd_set read_fds;
    struct timeval tv;

    while (m_bRunFlag.load(std::memory_order_acquire)) {
        ReapClientThreads();
        FD_ZERO(&read_fds);
        FD_SET(m_server_fd, &read_fds);

        int max_fd = m_server_fd;
        if (m_tlsServerFd >= 0) {
            FD_SET(m_tlsServerFd, &read_fds);
            if (m_tlsServerFd > max_fd) max_fd = m_tlsServerFd;
        }

        tv.tv_sec = 0;
        tv.tv_usec = 50000; // 50ms超时

        int ret = select(max_fd + 1, &read_fds, NULL, NULL, &tv);
        if (ret < 0) {
            if (errno != EINTR) {
                CLOG_ERR("WebSocket select错误: %s\n", strerror(errno));
            }
            continue;
        } else if (ret == 0) {
            continue;
        }

        // 处理新连接：明文 WS / wss 共用 ProcessClient，差别仅在握手前是否有 SSL_accept
        auto handleAccept = [this](int listenFd, bool isTls) {
            struct sockaddr_storage client_addr;   // 兼容 IPv4/IPv6 客户端地址
            socklen_t addr_len = sizeof(client_addr);
            int new_fd = accept(listenFd, (struct sockaddr *)&client_addr, &addr_len);
            if (new_fd < 0) return;
            if (!m_bRunFlag.load(std::memory_order_acquire)) {
                close(new_fd);
                return;
            }

            {
                std::lock_guard<std::mutex> lock(m_clientsMutex);
                if (m_clients.size() >= m_maxClients) {
                    CLOG_ERR("WebSocket reject fd=%d: clients=%zu >= max=%zu\n",
                             new_fd, m_clients.size(), m_maxClients);
                    close(new_fd);
                    return;
                }
            }

            // 设置非阻塞
            int flags = fcntl(new_fd, F_GETFL, 0);
            fcntl(new_fd, F_SETFL, flags | O_NONBLOCK);

            // TCP_NODELAY：关闭 Nagle 算法
            //   直播 fragment 通常几十 KB，且我们以 ~1Hz IDR 的节奏逐片推送；
            //   Nagle 默认在 ACK 未到时合并小包，会让某些 fragment
            //   多等 40ms（ACK 延迟）。对一个目标低延迟（< 1s）的直播链路而言，
            //   Nagle 累积的抖动是可观测的；m_isLive 路径必须关掉。
            //   非直播路径（回放 ws）也开着没坏处——回放本身就是一次性大块发送。
            {
                int one = 1;
                if (setsockopt(new_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) < 0) {
                    CLOG_ERR("setsockopt(TCP_NODELAY) fd=%d failed: %s\n",
                             new_fd, strerror(errno));
                }
            }

            try {
                std::shared_ptr<C_SslConn> sslConn;
                if (isTls) {
                    auto u = m_pTls->AcceptOnFd(new_fd);
                    if (!u) {
                        close(new_fd);
                        return;
                    }
                    sslConn = std::shared_ptr<C_SslConn>(std::move(u));
                }
                if (!m_bRunFlag.load(std::memory_order_acquire)) {
                    close(new_fd);
                    return;
                }

                auto wsClient = std::make_shared<WSClient>(new_fd);
                wsClient->ssl = sslConn;
                {
                    std::lock_guard<std::mutex> lock(m_clientsMutex);
                    m_clientFds.insert(new_fd);
                    m_clients[new_fd] = wsClient;
                }

                CLOG_INF("WebSocket%s新客户端连接: fd=%d\n",
                         isTls ? "(TLS)" : "", new_fd);

                auto done = std::make_shared<std::atomic<bool>>(false);
                ClientThread worker;
                worker.done = done;
                worker.thread = std::thread([this, new_fd, done]() {
                    try {
                        ProcessClient(new_fd);
                    } catch (const std::exception& e) {
                        CLOG_ERR("WebSocket client fd=%d exception: %s\n",
                                 new_fd, e.what());
                        if (m_pListener) m_pListener->OnWSClientDisconnect(new_fd);
                        CloseClient(new_fd);
                    } catch (...) {
                        CLOG_ERR("WebSocket client fd=%d unknown exception\n", new_fd);
                        if (m_pListener) m_pListener->OnWSClientDisconnect(new_fd);
                        CloseClient(new_fd);
                    }
                    done->store(true, std::memory_order_release);
                });
                m_clientThreads.push_back(std::move(worker));
            } catch (...) {
                if (GetClient(new_fd)) CloseClient(new_fd);
                else close(new_fd);
                CLOG_ERR("WebSocket client线程创建失败: fd=%d\n", new_fd);
            }
        };

        if (FD_ISSET(m_server_fd, &read_fds)) {
            handleAccept(m_server_fd, false);
        }
        if (m_tlsServerFd >= 0 && FD_ISSET(m_tlsServerFd, &read_fds)) {
            handleAccept(m_tlsServerFd, true);
        }
    }
    ReapClientThreads();
}

void C_WebSocketServer::ReapClientThreads()
{
    auto it = m_clientThreads.begin();
    while (it != m_clientThreads.end()) {
        if (!it->done || !it->done->load(std::memory_order_acquire)) {
            ++it;
            continue;
        }
        if (it->thread.joinable()) it->thread.join();
        it = m_clientThreads.erase(it);
    }
}

void C_WebSocketServer::JoinClientThreads()
{
    for (auto& worker : m_clientThreads) {
        if (worker.thread.joinable()) worker.thread.join();
    }
    m_clientThreads.clear();
}

int C_WebSocketServer::IoRead(WSClient* client, void* buf, int len, bool& wantMore)
{
    wantMore = false;
    if (!client) return -1;
    if (client->ssl) {
        return client->ssl->Read(buf, len, wantMore);
    }
    ssize_t n = recv(client->fd, buf, len, 0);
    if (n >= 0) return (int)n;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        wantMore = true;
        return -1;
    }
    return -1;
}

int C_WebSocketServer::IoWrite(WSClient* client, const void* buf, int len, bool& wantMore)
{
    wantMore = false;
    if (!client) return -1;
    if (client->ssl) {
        return client->ssl->Write(buf, len, wantMore);
    }
    ssize_t n = send(client->fd, buf, len, MSG_NOSIGNAL);
    if (n >= 0) return (int)n;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        wantMore = true;
        return -1;
    }
    return -1;
}

void C_WebSocketServer::ProcessClient(int fd)
{
    std::vector<unsigned char> buffer(4096);
    std::vector<unsigned char> recvData;
    recvData.reserve(kMaxHandshakeBytes);
    const auto connectedAt = std::chrono::steady_clock::now();
    std::atomic<bool> fileRun{true};
    std::thread fileThread;
    struct FileThreadGuard {
        std::atomic<bool>& run;
        std::thread& thread;
        ~FileThreadGuard() {
            run.store(false, std::memory_order_release);
            if (thread.joinable()) thread.join();
        }
    } fileThreadGuard{fileRun, fileThread};

    // 回放模式的文件读取和发送逻辑
    if (!m_isLive) {
        // 旧回放协议需要并行读控制帧和发文件，但辅助线程必须归属于本连接，
        // 连接退出前会停止并 join，不能越过 server/WSClient 生命周期。
        fileThread = std::thread([this, fd, &fileRun]() {
            std::ifstream sendFile;
            std::vector<long long> idrPos100(100);
            int intervalMs = 30; // 默认播放间隔

            // 打开第一个视频文件
            std::string videoDir = "video/";
            DIR *dir = opendir(videoDir.c_str());
            if (dir) {
                struct dirent *entry;
                std::string firstFile;
                while ((entry = readdir(dir)) != nullptr) {
                    if (entry->d_type == DT_REG) {
                        firstFile = videoDir + entry->d_name;
                        break;
                    }
                }
                closedir(dir);

                if (!firstFile.empty()) {
                    sendFile.open(firstFile, std::ios::binary);
                    if (sendFile.is_open()) {
                        // 读取I帧位置信息
                        sendFile.read(reinterpret_cast<char*>(idrPos100.data()), idrPos100.size() * sizeof(long long));

                        while (fileRun.load(std::memory_order_acquire) &&
                               m_bRunFlag.load(std::memory_order_acquire) &&
                               GetClient(fd)) {
                            // 读取数据长度
                            unsigned int allDataLen = 0;
                            sendFile.read((char*)&allDataLen, sizeof(allDataLen));
                            if (sendFile.eof()) {
                                // 文件读取完毕，重新打开
                                sendFile.close();
                                sendFile.open(firstFile, std::ios::binary);
                                if (sendFile.is_open()) {
                                    sendFile.read(reinterpret_cast<char*>(idrPos100.data()), idrPos100.size() * sizeof(long long));
                                    continue;
                                } else {
                                    break;
                                }
                            }
                            if (allDataLen == 0 || allDataLen > 512 * 1024) {
                                CLOG_ERR("回放文件帧长度非法: %u\n", allDataLen);
                                break;
                            }

                            // 读取数据
                            std::vector<char> dataBuffer(allDataLen);
                            sendFile.read(dataBuffer.data(), allDataLen);
                            if (sendFile.eof()) {
                                continue;
                            }

                            // 构建数据包并发送
                            if (allDataLen > 0) {
                                // 文件中存储的格式：[1字节flag] + [数据]
                                // 直播模式格式：[1字节flag(0x00/0x80)] + [数据]
                                // 需要转换flag值：0->0x00, 1->0x80
                                char fileFlag = dataBuffer[0];
                                char wsFlag = (fileFlag == 1) ? 0x80 : 0x00;
                                unsigned int payloadLen = allDataLen - 1;
                                
                                // 构建与直播模式相同格式的数据包
                                unsigned int totalLen = sizeof(unsigned int) + 1 + payloadLen;
                                std::vector<unsigned char> packet(totalLen);
                                
                                // 写入长度（网络字节序）
                                unsigned int netLen = htonl(payloadLen + 1);
                                memcpy(packet.data(), &netLen, sizeof(netLen));
                                
                                // 写入flag
                                packet[sizeof(netLen)] = wsFlag;
                                
                                // 写入数据（跳过文件中的flag字节）
                                memcpy(packet.data() + sizeof(netLen) + 1, dataBuffer.data() + 1, payloadLen);
                                
                                if (SendBinary(fd, packet.data(), packet.size()) < 0 &&
                                    !GetClient(fd)) {
                                    break;
                                }
                            }

                            // 视频帧间隔
                            std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));
                        }
                    }
                }
            }
        });
    }

    while (m_bRunFlag.load(std::memory_order_acquire)) {
        auto clientForRead = GetClient(fd);
        if (!clientForRead) break;
        if (!clientForRead->handshakeComplete &&
            std::chrono::steady_clock::now() - connectedAt > kHandshakeTimeout) {
            CLOG_ERR("WebSocket握手超时: fd=%d\n", fd);
            break;
        }

        // 直播广播只负责入队，所有实际 socket/SSL 写均在本客户端线程串行执行。
        if (clientForRead->handshakeComplete && DrainSendQueue(fd) < 0) {
            CLOG_ERR("WebSocket待发队列发送失败: fd=%d\n", fd);
            break;
        }

        bool wantMore = false;
        int n = IoRead(clientForRead.get(), buffer.data(), (int)buffer.size(), wantMore);

        if (n < 0) {
            if (wantMore) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            CLOG_ERR("WebSocket recv错误: %s\n", strerror(errno));
            break;
        } else if (n == 0) {
            // 客户端断开
            CLOG_INF("WebSocket客户端断开: fd=%d\n", fd);
            break;
        }

        const size_t recvLimit = clientForRead->handshakeComplete
            ? (kMaxClientPayload + 14)
            : kMaxHandshakeBytes;
        if ((size_t)n > recvLimit - std::min(recvData.size(), recvLimit)) {
            CLOG_ERR("WebSocket接收缓存超限: fd=%d limit=%zu\n", fd, recvLimit);
            break;
        }
        recvData.insert(recvData.end(), buffer.begin(), buffer.begin() + n);

        auto client = GetClient(fd);
        if (!client) break;

        if (!client->handshakeComplete) {
            static const unsigned char kHeaderEnd[] = {'\r', '\n', '\r', '\n'};
            const bool headerComplete =
                std::search(recvData.begin(), recvData.end(),
                            kHeaderEnd, kHeaderEnd + sizeof(kHeaderEnd)) != recvData.end();
            // 处理握手
            if (HandleHandshake(fd, recvData)) {
                recvData.clear();
                client->handshakeComplete = true;

                // 必须在状态切到 OPEN 之前同步发完 init segment。若先 OPEN，编码线程
                // 可能抢先广播 fragment，手机端便会把 fragment 误当 init；两线程还会
                // 同时 write 同一 fd，造成 WebSocket 帧字节交叉。
                if (m_isLive &&
                    client->urlPath != "/ws/audio" &&
                    client->urlPath != "/ws/audio/" &&
                    client->urlPath != "/ws/talk" &&
                    client->urlPath != "/ws/talk/" &&
                    client->urlPath != "/ws/log" &&
                    client->urlPath != "/ws/log/") {
                    std::vector<uint8_t> initSegCopy;
                    {
                        std::lock_guard<std::mutex> lock(m_initSegMutex);
                        initSegCopy = m_initSegCache;
                    }
                    if (!initSegCopy.empty()) {
                        if (SendWSFrame(fd, initSegCopy.data(),
                                        (unsigned int)initSegCopy.size(), WS_BINARY) < 0) {
                            goto client_exit;
                        }
                    }
                }

                {
                    std::lock_guard<std::mutex> lock(m_clientsMutex);
                    auto it = m_clients.find(fd);
                    if (it == m_clients.end() || it->second.get() != client.get()) {
                        goto client_exit;
                    }
                    client->state = WS_OPEN;
                }

                // 通知新客户端连接
                if (m_pListener) {
                    m_pListener->OnNewWSClientConnect(fd);
                }
            } else if (headerComplete) {
                CLOG_ERR("WebSocket握手内容非法: fd=%d\n", fd);
                break;
            }
        } else {
            // 处理WebSocket帧
            while (!recvData.empty()) {
                std::vector<unsigned char> payload;
                WSOpcode opcode;

                const ParseResult parseResult =
                    ParseWSFrame(fd, recvData, payload, opcode);
                if (parseResult == ParseResult::Incomplete) {
                    break; // 数据不足，等待更多数据
                }
                if (parseResult == ParseResult::ProtocolError) {
                    CLOG_ERR("WebSocket协议错误: fd=%d\n", fd);
                    goto client_exit;
                }

                // 处理帧
                switch (opcode) {
                    case WS_TEXT:
                    case WS_BINARY:
                        if (m_pListener) {
                            m_pListener->OnWSClientMessage(fd, payload);
                        }
                        break;

                    case WS_CLOSE:
                        CLOG_INF("WebSocket收到关闭帧: fd=%d\n", fd);
                        goto client_exit;

                    case WS_PING:
                        // 发送Pong响应
                        SendWSFrame(fd, payload.data(), payload.size(), WS_PONG);
                        break;

                    case WS_PONG:
                        // 忽略Pong响应
                        break;

                    default:
                        break;
                }
            }
        }
    }

client_exit:
    fileRun.store(false, std::memory_order_release);
    if (fileThread.joinable()) fileThread.join();

    // 通知客户端断开
    if (m_pListener) {
        m_pListener->OnWSClientDisconnect(fd);
    }
    CloseClient(fd);
}

bool C_WebSocketServer::HandleHandshake(int fd, const std::vector<unsigned char>& data)
{
    std::string request(data.begin(), data.end());

    // 查找HTTP请求结束标记
    if (request.find("\r\n\r\n") == std::string::npos) {
        return false; // 数据不完整
    }

    // 解析请求行中的 URL path（"GET /ws/talk HTTP/1.1"）
    std::string urlPath = "/";
    {
        size_t lineEnd = request.find("\r\n");
        if (lineEnd != std::string::npos) {
            std::string line = request.substr(0, lineEnd);
            size_t p1 = line.find(' ');
            size_t p2 = (p1 != std::string::npos) ? line.find(' ', p1 + 1) : std::string::npos;
            if (p1 != std::string::npos && p2 != std::string::npos && p2 > p1 + 1) {
                urlPath = line.substr(p1 + 1, p2 - p1 - 1);
                // 去掉 query
                size_t q = urlPath.find('?');
                if (q != std::string::npos) urlPath = urlPath.substr(0, q);
            }
        }
    }

    // 解析Sec-WebSocket-Key
    size_t keyPos = request.find("Sec-WebSocket-Key: ");
    if (keyPos == std::string::npos) {
        CLOG_ERR("WebSocket握手失败: 未找到Sec-WebSocket-Key\n");
        return false;
    }

    keyPos += strlen("Sec-WebSocket-Key: ");
    size_t keyEnd = request.find("\r\n", keyPos);
    std::string key = request.substr(keyPos, keyEnd - keyPos);

    // 计算Accept Key
    std::string acceptKey = CalcAcceptKey(key);

    // 构建握手响应
    std::string response =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " + acceptKey + "\r\n"
        "\r\n";

    // 通过 IoWrite 发送响应（同时支持 ws/wss）
    auto client = GetClient(fd);
    if (!client) {
        CLOG_ERR("WebSocket握手响应失败: fd=%d 已无对应客户端\n", fd);
        return false;
    }
    client->urlPath = urlPath;

    const char* p = response.c_str();
    size_t remain = response.length();
    const auto writeDeadline = std::chrono::steady_clock::now() + kHandshakeTimeout;
    while (remain > 0) {
        bool wantMore = false;
        int w = IoWrite(client.get(), p, (int)remain, wantMore);
        if (w > 0) {
            p      += w;
            remain -= (size_t)w;
            continue;
        }
        if (wantMore) {
            if (std::chrono::steady_clock::now() >= writeDeadline) {
                CLOG_ERR("WebSocket握手响应超时(fd=%d)\n", fd);
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        CLOG_ERR("WebSocket握手响应发送失败(fd=%d): %s\n", fd, strerror(errno));
        return false;
    }

    CLOG_INF("WebSocket握手成功: fd=%d path=%s\n", fd, urlPath.c_str());

    if (m_pListener) {
        m_pListener->OnWSClientHandshake(fd, urlPath);
    }
    return true;
}

std::string C_WebSocketServer::CalcAcceptKey(const std::string& key)
{
    std::string combined = key + WS_GUID;

    // 使用简单SHA1实现
    std::string hash = SimpleSHA1::hash(combined);

    // 使用简单Base64实现
    return SimpleBase64::encode(hash);
}

C_WebSocketServer::ParseResult
C_WebSocketServer::ParseWSFrame(int fd, std::vector<unsigned char>& data,
                                std::vector<unsigned char>& payload, WSOpcode& opcode)
{
    (void)fd;
    if (data.size() < 2) return ParseResult::Incomplete;

    // 解析帧头
    bool fin = (data[0] & 0x80) != 0;
    opcode = static_cast<WSOpcode>(data[0] & 0x0F);
    bool masked = (data[1] & 0x80) != 0;
    unsigned long long payloadLen = data[1] & 0x7F;

    // 当前轻量实现不做分片重组；客户端到服务端的帧按 RFC 6455 必须带 mask。
    if (!fin || (data[0] & 0x70) != 0 || !masked) {
        return ParseResult::ProtocolError;
    }
    switch (opcode) {
        case WS_TEXT:
        case WS_BINARY:
        case WS_CLOSE:
        case WS_PING:
        case WS_PONG:
            break;
        default:
            return ParseResult::ProtocolError;
    }

    size_t headerLen = 2;

    // 解析扩展长度
    if (payloadLen == 126) {
        if (data.size() < 4) return ParseResult::Incomplete;
        payloadLen = (data[2] << 8) | data[3];
        headerLen = 4;
    } else if (payloadLen == 127) {
        if (data.size() < 10) return ParseResult::Incomplete;
        payloadLen = 0;
        for (int i = 0; i < 8; i++) {
            payloadLen = (payloadLen << 8) | data[2 + i];
        }
        headerLen = 10;
    }
    if (payloadLen > kMaxClientPayload ||
        (opcode >= WS_CLOSE && payloadLen > 125)) {
        return ParseResult::ProtocolError;
    }

    // 解析掩码
    unsigned char mask[4] = {0};
    if (data.size() < headerLen + 4) return ParseResult::Incomplete;
    memcpy(mask, &data[headerLen], 4);
    headerLen += 4;

    // 先做减法再比较，禁止 headerLen + payloadLen 的整数回绕。
    if (payloadLen > data.size() - headerLen) return ParseResult::Incomplete;
    const size_t payloadSize = static_cast<size_t>(payloadLen);

    // 提取payload
    payload.resize(payloadSize);
    for (size_t i = 0; i < payloadSize; i++) {
        payload[i] = data[headerLen + i];
        payload[i] ^= mask[i % 4];
    }

    // 从data中移除已处理的帧
    data.erase(data.begin(), data.begin() + headerLen + payloadSize);

    return ParseResult::Complete;
}

std::vector<unsigned char> C_WebSocketServer::BuildWSFrame(const unsigned char* data,
                                                            unsigned int len, WSOpcode opcode)
{
    std::vector<unsigned char> frame;

    // 帧头
    frame.push_back(0x80 | static_cast<unsigned char>(opcode)); // FIN=1, opcode

    // 长度
    if (len < 126) {
        frame.push_back(static_cast<unsigned char>(len));
    } else if (len < 65536) {
        frame.push_back(126);
        frame.push_back((len >> 8) & 0xFF);
        frame.push_back(len & 0xFF);
    } else {
        frame.push_back(127);
        const uint64_t len64 = len;
        for (int i = 7; i >= 0; i--) {
            frame.push_back((len64 >> (i * 8)) & 0xFF);
        }
    }

    // payload
    frame.insert(frame.end(), data, data + len);

    return frame;
}

int C_WebSocketServer::SendWSFrame(int fd, const unsigned char* data, unsigned int len, WSOpcode opcode)
{
    auto frame = BuildWSFrame(data, len, opcode);

    auto client = GetClient(fd);
    if (!client) return -1;

    std::lock_guard<std::mutex> writeLock(client->writeMutex);

    // 必须循环发送直到全部写完：客户端 socket 是非阻塞模式（O_NONBLOCK），
    // fMP4 fragment 通常 10~30KB，一次 send 写不完会发生 short write。
    // wss 模式下还要处理 SSL_ERROR_WANT_READ/WRITE。
    const unsigned char* p = frame.data();
    size_t remaining = frame.size();
    while (remaining > 0) {
        bool wantMore = false;
        int sent = IoWrite(client.get(), p, (int)remaining, wantMore);
        if (sent > 0) {
            p         += sent;
            remaining -= (size_t)sent;
            continue;
        }
        if (wantMore) {
            // socket / SSL 缓冲满，等可写（最多 1s）
            fd_set wfds;
            FD_ZERO(&wfds);
            FD_SET(fd, &wfds);
            struct timeval tv = {1, 0};
            int sret = select(fd + 1, nullptr, &wfds, nullptr, &tv);
            if (sret <= 0) {
                CLOG_ERR("WebSocket发送超时(fd=%d remain=%zu)\n", fd, remaining);
                return -1;
            }
            continue;
        }
        // 其它错误（EPIPE/ECONNRESET/...）
        CLOG_ERR("WebSocket发送失败(fd=%d): %s\n", fd, strerror(errno));
        return -1;
    }
    return 0;
}

int C_WebSocketServer::SendBinary(int fd, unsigned char* pData, unsigned int nLen)
{
    auto client = GetClient(fd);
    if (!client || client->state != WS_OPEN) {
        return -1;
    }

    return SendWSFrame(fd, pData, nLen, WS_BINARY);
}

bool C_WebSocketServer::QueueBinary(int fd, const uint8_t* data, size_t len)
{
    if (!data || len == 0 || len > kMaxQueuedBytes) return false;

    std::shared_ptr<WSClient> client;
    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        auto it = m_clients.find(fd);
        if (it == m_clients.end() || !it->second || it->second->state != WS_OPEN) {
            return false;
        }
        client = it->second;
    }

    std::lock_guard<std::mutex> lock(client->sendQueueMutex);
    while (!client->sendQueue.empty() &&
           (client->sendQueue.size() >= kMaxQueuedFrames ||
            client->sendQueueBytes + len > kMaxQueuedBytes)) {
        client->sendQueueBytes -= client->sendQueue.front().size();
        client->sendQueue.pop_front();
    }
    client->sendQueue.emplace_back(data, data + len);
    client->sendQueueBytes += len;
    return true;
}

int C_WebSocketServer::DrainSendQueue(int fd)
{
    auto client = GetClient(fd);
    if (!client) return -1;

    // 每轮限制发送数量，避免纯音频队列持续有数据时饿死客户端的读/Ping 路径。
    for (size_t i = 0; i < kMaxQueuedFrames; ++i) {
        std::vector<unsigned char> payload;
        {
            std::lock_guard<std::mutex> lock(client->sendQueueMutex);
            if (client->sendQueue.empty()) break;
            payload = std::move(client->sendQueue.front());
            client->sendQueue.pop_front();
            client->sendQueueBytes -= payload.size();
        }
        if (SendWSFrame(fd, payload.data(), (unsigned int)payload.size(), WS_BINARY) < 0) {
            return -1;
        }
    }
    return 0;
}

void C_WebSocketServer::BroadcastBinary(const uint8_t* data, size_t len)
{
    // 生产线程仅复制到有界队列，绝不执行网络 IO。否则一个弱网手机就能让
    // H264Enc 回调阻塞，进而让相机帧无法及时释放并出现全局卡流。
    std::vector<int> snapshot;
    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        snapshot.reserve(m_clientFds.size());
        for (int fd : m_clientFds) {
            auto it = m_clients.find(fd);
            if (it == m_clients.end()) continue;
            auto& cli = it->second;
            if (cli && cli->state == WS_OPEN) {
                // 跳过纯音频订阅者：他们走 BroadcastAudioBinary
                if (cli->urlPath == "/ws/audio" || cli->urlPath == "/ws/audio/") continue;
                // 对讲连接只上传 OPUS，不应接收 fMP4。
                if (cli->urlPath == "/ws/talk" || cli->urlPath == "/ws/talk/") continue;
                // 日志订阅者只接收 /ws/log 文本，不应收到 fMP4。
                if (cli->urlPath == "/ws/log" || cli->urlPath == "/ws/log/") continue;
                snapshot.push_back(fd);
            }
        }
    }
    for (int fd : snapshot) {
        QueueBinary(fd, data, len);
    }
}

void C_WebSocketServer::BroadcastAudioBinary(const uint8_t* data, size_t len)
{
    // 仅向 /ws/audio 路径下的客户端广播，同样只入有界队列。
    std::vector<int> snapshot;
    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        snapshot.reserve(m_clientFds.size());
        for (int fd : m_clientFds) {
            auto it = m_clients.find(fd);
            if (it == m_clients.end()) continue;
            auto& cli = it->second;
            if (!cli || cli->state != WS_OPEN) continue;
            if (cli->urlPath == "/ws/audio" || cli->urlPath == "/ws/audio/") {
                snapshot.push_back(fd);
            }
        }
    }
    for (int fd : snapshot) {
        QueueBinary(fd, data, len);
    }
}

void C_WebSocketServer::BroadcastLogText(const char* data, size_t len)
{
    // 仅向 /ws/log 路径下的客户端广播。用 binary 帧承载 UTF-8 日志文本，复用
    // 有界发送队列，弱网客户端不会反压推送线程。
    if (!data || len == 0) return;
    std::vector<int> snapshot;
    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        snapshot.reserve(m_clientFds.size());
        for (int fd : m_clientFds) {
            auto it = m_clients.find(fd);
            if (it == m_clients.end()) continue;
            auto& cli = it->second;
            if (!cli || cli->state != WS_OPEN) continue;
            if (cli->urlPath == "/ws/log" || cli->urlPath == "/ws/log/") {
                snapshot.push_back(fd);
            }
        }
    }
    for (int fd : snapshot) {
        QueueBinary(fd, reinterpret_cast<const uint8_t*>(data), len);
    }
}

void C_WebSocketServer::OnLiveInitSegment(const uint8_t* data, size_t len)
{
    // 1) 缓存（新连接握手成功后立即下发，保证任意时刻接入都能解码）
    {
        std::lock_guard<std::mutex> lock(m_initSegMutex);
        m_initSegCache.assign(data, data + len);
    }
    // 2) 同时广播给已连接客户端（init segment 重置时）
    BroadcastBinary(data, len);
}

void C_WebSocketServer::OnLiveFragment(const uint8_t* data, size_t len)
{
    BroadcastBinary(data, len);
}

void C_WebSocketServer::CloseClient(int fd)
{
    std::lock_guard<std::mutex> lock(m_clientsMutex);

    auto it = m_clients.find(fd);
    if (it != m_clients.end()) {
        auto& cli = it->second;
        if (!cli) {
            close(fd);
            m_clientFds.erase(fd);
            m_clients.erase(it);
            return;
        }
        // 发送关闭帧（明文/TLS 都要）
        unsigned char closeFrame[] = {0x88, 0x00}; // FIN=1, opcode=CLOSE
        std::lock_guard<std::mutex> writeLock(cli->writeMutex);
        if (cli->ssl) {
            bool wm = false;
            cli->ssl->Write(closeFrame, sizeof(closeFrame), wm);
            cli->ssl->Shutdown();
        } else {
            send(fd, closeFrame, sizeof(closeFrame), MSG_NOSIGNAL);
        }

        close(fd);
        m_clientFds.erase(fd);
        m_clients.erase(it);
    }
}

std::shared_ptr<WSClient> C_WebSocketServer::GetClient(int fd)
{
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    auto it = m_clients.find(fd);
    if (it != m_clients.end()) {
        return it->second;
    }
    return nullptr;
}

int C_WebSocketServer::GetClientCount()
{
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    int count = 0;
    for (const auto& pair : m_clients) {
        if (pair.second->state == WS_OPEN) {
            count++;
        }
    }
    return count;
}
