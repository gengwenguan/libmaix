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

    WSClient(int socketFd) : fd(socketFd), state(WS_CONNECTING),
        handshakeComplete(false) {}
};

class C_WebSocketServer
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
        /*接收到客户端消息*/
        virtual int OnWSClientMessage(int fd, const std::vector<unsigned char>& data) = 0;
    };

public:
    C_WebSocketServer(C_Listener* pListener, int port, bool isLive);
    ~C_WebSocketServer();

    // 发送H264视频数据给所有连接的客户端
    int SendH264(unsigned char* pData, unsigned int nLen);

    // 发送Opus音频数据给所有连接的客户端
    int SendOpus(unsigned char* pData, unsigned int nLen);

    // 发送二进制数据给指定客户端
    int SendBinary(int fd, unsigned char* pData, unsigned int nLen);

    // 获取当前连接数
    int GetClientCount();

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
};
