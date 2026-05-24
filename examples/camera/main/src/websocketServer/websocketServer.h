/*********************************************************************************
  *Copyright(C),Your Company
  *FileName:  websocketServer.h
  *Author:    gengwenguan
  *Date:      2024-12-01
  *Description:  WebSocket服务器，用于支持Web浏览器客户端连接
                 实现轻量级WebSocket协议，支持直播和回放
**********************************************************************************/
#pragma once
#include <iostream>
#include <thread>
#include <mutex>
#include <set>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "liveHub.h"

class C_TlsContext;
class C_SslConn;

// WebSocket帧操作码
enum WSOpcode {
    WS_CONTINUATION = 0x0,
    WS_TEXT = 0x1,
    WS_BINARY = 0x2,
    WS_CLOSE = 0x8,
    WS_PING = 0x9,
    WS_PONG = 0xA
};

// WebSocket连接状态
enum WSState {
    WS_CONNECTING,
    WS_OPEN,
    WS_CLOSING,
    WS_CLOSED
};

// WebSocket客户端连接
struct WSClient {
    int fd;
    WSState state;
    std::vector<unsigned char> recvBuffer;
    std::vector<unsigned char> sendBuffer;
    bool handshakeComplete;
    std::shared_ptr<C_SslConn> ssl;  // 非空 = wss 连接，走 SSL_*

    // 握手时解析出的 URL path（如 "/ws/talk"），由 listener 区分子端点
    std::string urlPath;

    WSClient(int socketFd) : fd(socketFd), state(WS_CONNECTING),
        handshakeComplete(false) {}
};

class C_WebSocketServer : public C_LiveHub::C_Listener
{
public:
    class C_Listener
    {
    public:
        virtual ~C_Listener() = default;
        /*新客户端连接事件*/
        virtual int OnNewWSClientConnect(int fd) = 0;
        /*客户端断开连接事件*/
        virtual int OnWSClientDisconnect(int fd) = 0;
        /*接收到客户端消息（path 用于区分 endpoint，例如 /ws/talk）*/
        virtual int OnWSClientMessage(int fd, const std::vector<unsigned char>& data) = 0;
        /*握手完成时回调，可获取 URL path（默认实现为空，子类可选实现）*/
        virtual void OnWSClientHandshake(int /*fd*/, const std::string& /*urlPath*/) {}
    };

public:
    // isLive=true: 直播模式，自动订阅 LiveHub，将 fMP4 init/fragment 广播给所有客户端
    // isLive=false: 回放模式，从 video/ 目录读取文件按帧发送
    C_WebSocketServer(C_Listener* pListener, int port, bool isLive);
    ~C_WebSocketServer();

    // 可选：启用 wss 端口（在构造之后、继续工作前调用一次）。
    // 调用后将额外监听 tlsPort，TLS 握手成功后走相同的 WS 流程。
    void EnableTls(int tlsPort, C_TlsContext* pTls);

    // 发送二进制数据给指定客户端（fMP4 字节直接通过 WS binary 帧）
    int SendBinary(int fd, unsigned char* pData, unsigned int nLen);

    // 获取当前连接数
    int GetClientCount();

    // C_LiveHub::C_Listener
    // init segment：缓存 + 广播给所有已握手客户端
    void OnLiveInitSegment(const uint8_t* data, size_t len) override;
    // fragment：广播给所有已握手客户端
    void OnLiveFragment(const uint8_t* data, size_t len) override;

private:
    // 接收客户端连接线程
    void AcceptThread();

    // 处理客户端数据线程
    void ProcessClient(int fd);

    // 处理WebSocket握手
    bool HandleHandshake(int fd, const std::vector<unsigned char>& data);

    // 解析WebSocket帧
    bool ParseWSFrame(int fd, std::vector<unsigned char>& data, 
                      std::vector<unsigned char>& payload, WSOpcode& opcode);

    // 构建WebSocket帧
    std::vector<unsigned char> BuildWSFrame(const unsigned char* data,
                                             unsigned int len, WSOpcode opcode);

    // 计算WebSocket握手响应的Accept Key
    std::string CalcAcceptKey(const std::string& key);

    // 发送WebSocket帧
    int SendWSFrame(int fd, const unsigned char* data, unsigned int len, WSOpcode opcode);

    // 关闭客户端连接
    void CloseClient(int fd);

    // 获取客户端
    std::shared_ptr<WSClient> GetClient(int fd);

    // 内部 IO 包装：根据 client->ssl 是否非空走 SSL 或 plain；wantMore=true ⇒ 类似 EAGAIN
    int IoRead (WSClient* client, void* buf, int len, bool& wantMore);
    int IoWrite(WSClient* client, const void* buf, int len, bool& wantMore);

private:
    C_Listener* m_pListener;
    int m_port;
    bool m_isLive;  // true=直播, false=回放

    int m_server_fd;
    bool m_bRunFlag;
    std::thread m_acceptThread;

    std::mutex m_clientsMutex;
    std::set<int> m_clientFds;
    std::map<int, std::shared_ptr<WSClient>> m_clients;

    // ---- wss（可选）----
    int           m_tlsPort     = 0;
    int           m_tlsServerFd = -1;
    C_TlsContext* m_pTls        = nullptr;

    // 缓存的 fMP4 init segment（ftyp+moov），新客户端握手成功后立即下发
    std::mutex            m_initSegMutex;
    std::vector<uint8_t>  m_initSegCache;

    // 直播模式下广播 binary 给所有已 OPEN 的客户端
    void BroadcastBinary(const uint8_t* data, size_t len);
};
