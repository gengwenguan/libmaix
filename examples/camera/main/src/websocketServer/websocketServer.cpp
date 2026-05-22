/*********************************************************************************
  *Copyright(C),Your Company
  *FileName:  websocketServer.cpp
  *Author:    gengwenguan
  *Date:      2024-12-01
  *Description:  WebSocket服务器实现（不依赖OpenSSL）
**********************************************************************************/
#include "websocketServer.h"
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
#include <fcntl.h>
#include <chrono>
#include <fstream>
#include <dirent.h>

// WebSocket GUID (RFC 6455)
static const char WS_GUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

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
    , m_bRunFlag(true)
{
    // 创建socket
    m_server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (m_server_fd < 0) {
        CLOG_ERR("WebSocket socket创建失败: %s\n", strerror(errno));
        return;
    }

    // 设置地址重用
    int opt = 1;
    if (setsockopt(m_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        CLOG_ERR("WebSocket setsockopt失败: %s\n", strerror(errno));
        close(m_server_fd);
        m_server_fd = -1;
        return;
    }

    // 设置非阻塞模式
    int flags = fcntl(m_server_fd, F_GETFL, 0);
    fcntl(m_server_fd, F_SETFL, flags | O_NONBLOCK);

    // 绑定地址
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(m_port);

    if (bind(m_server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        CLOG_ERR("WebSocket bind失败: %s\n", strerror(errno));
        close(m_server_fd);
        m_server_fd = -1;
        return;
    }

    // 监听
    if (listen(m_server_fd, 10) < 0) {
        CLOG_ERR("WebSocket listen失败: %s\n", strerror(errno));
        close(m_server_fd);
        m_server_fd = -1;
        return;
    }

    CLOG_INF("WebSocket服务器启动在端口 %d (%s)\n", m_port, m_isLive ? "直播" : "回放");

    // 启动接收线程
    m_acceptThread = std::thread(&C_WebSocketServer::AcceptThread, this);
}

C_WebSocketServer::~C_WebSocketServer()
{
    m_bRunFlag = false;

    if (m_acceptThread.joinable()) {
        m_acceptThread.join();
    }

    // 关闭所有客户端连接
    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        for (int fd : m_clientFds) {
            close(fd);
        }
        m_clientFds.clear();
        m_clients.clear();
    }

    if (m_server_fd >= 0) {
        close(m_server_fd);
    }

    CLOG_INF("WebSocket服务器停止\n");
}

void C_WebSocketServer::AcceptThread()
{
    fd_set read_fds;
    struct timeval tv;

    while (m_bRunFlag) {
        FD_ZERO(&read_fds);
        FD_SET(m_server_fd, &read_fds);

        // 添加所有客户端到fd_set
        int max_fd = m_server_fd;
        {
            std::lock_guard<std::mutex> lock(m_clientsMutex);
            for (int fd : m_clientFds) {
                FD_SET(fd, &read_fds);
                if (fd > max_fd) max_fd = fd;
            }
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

        // 处理新连接
        if (FD_ISSET(m_server_fd, &read_fds)) {
            struct sockaddr_in client_addr;
            socklen_t addr_len = sizeof(client_addr);
            int new_fd = accept(m_server_fd, (struct sockaddr *)&client_addr, &addr_len);

            if (new_fd >= 0) {
                // 设置非阻塞
                int flags = fcntl(new_fd, F_GETFL, 0);
                fcntl(new_fd, F_SETFL, flags | O_NONBLOCK);

                // 添加到客户端集合
                {
                    std::lock_guard<std::mutex> lock(m_clientsMutex);
                    m_clientFds.insert(new_fd);
                    m_clients[new_fd] = std::make_shared<WSClient>(new_fd);
                }

                CLOG_INF("WebSocket新客户端连接: fd=%d\n", new_fd);

                // 启动客户端处理线程
                std::thread clientThread(&C_WebSocketServer::ProcessClient, this, new_fd);
                clientThread.detach();
            }
        }
    }
}

void C_WebSocketServer::ProcessClient(int fd)
{
    std::vector<unsigned char> buffer(4096);
    std::vector<unsigned char> recvData;

    // 回放模式的文件读取和发送逻辑
    if (!m_isLive) {
        // 启动文件读取线程
        std::thread fileThread([this, fd]() {
            std::ifstream sendFile;
            std::vector<long long> idrPos100(100);
            char progress = 0;
            int intervalMs = 30; // 默认播放间隔
            bool runFlag = true;

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

                        while (runFlag) {
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
                                
                                SendBinary(fd, packet.data(), packet.size());
                            }

                            // 视频帧间隔
                            std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));
                        }
                    }
                }
            }
        });
        fileThread.detach();
    }

    while (m_bRunFlag) {
        ssize_t n = recv(fd, buffer.data(), buffer.size(), 0);

        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
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

        recvData.insert(recvData.end(), buffer.begin(), buffer.begin() + n);

        auto client = GetClient(fd);
        if (!client) break;

        if (!client->handshakeComplete) {
            // 处理握手
            if (HandleHandshake(fd, recvData)) {
                recvData.clear();
                client->handshakeComplete = true;
                client->state = WS_OPEN;

                // 通知新客户端连接
                if (m_pListener) {
                    m_pListener->OnNewWSClientConnect(fd);
                }
            }
        } else {
            // 处理WebSocket帧
            while (!recvData.empty()) {
                std::vector<unsigned char> payload;
                WSOpcode opcode;

                if (!ParseWSFrame(fd, recvData, payload, opcode)) {
                    break; // 数据不足，等待更多数据
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

    // 发送响应
    ssize_t sent = send(fd, response.c_str(), response.length(), MSG_NOSIGNAL);
    if (sent < 0) {
        CLOG_ERR("WebSocket握手响应发送失败: %s\n", strerror(errno));
        return false;
    }

    CLOG_INF("WebSocket握手成功: fd=%d\n", fd);
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

bool C_WebSocketServer::ParseWSFrame(int fd, std::vector<unsigned char>& data, 
                                      std::vector<unsigned char>& payload, WSOpcode& opcode)
{
    if (data.size() < 2) return false;

    // 解析帧头
    bool fin = (data[0] & 0x80) != 0;
    opcode = static_cast<WSOpcode>(data[0] & 0x0F);
    bool masked = (data[1] & 0x80) != 0;
    unsigned long long payloadLen = data[1] & 0x7F;

    size_t headerLen = 2;

    // 解析扩展长度
    if (payloadLen == 126) {
        if (data.size() < 4) return false;
        payloadLen = (data[2] << 8) | data[3];
        headerLen = 4;
    } else if (payloadLen == 127) {
        if (data.size() < 10) return false;
        payloadLen = 0;
        for (int i = 0; i < 8; i++) {
            payloadLen = (payloadLen << 8) | data[2 + i];
        }
        headerLen = 10;
    }

    // 解析掩码
    unsigned char mask[4] = {0};
    if (masked) {
        if (data.size() < headerLen + 4) return false;
        memcpy(mask, &data[headerLen], 4);
        headerLen += 4;
    }

    // 检查数据是否完整
    if (data.size() < headerLen + payloadLen) return false;

    // 提取payload
    payload.resize(payloadLen);
    for (size_t i = 0; i < payloadLen; i++) {
        payload[i] = data[headerLen + i];
        if (masked) {
            payload[i] ^= mask[i % 4];
        }
    }

    // 从data中移除已处理的帧
    data.erase(data.begin(), data.begin() + headerLen + payloadLen);

    return true;
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
        for (int i = 7; i >= 0; i--) {
            frame.push_back((len >> (i * 8)) & 0xFF);
        }
    }

    // payload
    frame.insert(frame.end(), data, data + len);

    return frame;
}

int C_WebSocketServer::SendWSFrame(int fd, const unsigned char* data, unsigned int len, WSOpcode opcode)
{
    auto frame = BuildWSFrame(data, len, opcode);

    ssize_t sent = send(fd, frame.data(), frame.size(), MSG_NOSIGNAL);
    if (sent < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            CLOG_ERR("WebSocket发送失败: %s\n", strerror(errno));
            return -1;
        }
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

int C_WebSocketServer::SendH264(unsigned char* pData, unsigned int nLen)
{
    // 构建数据包: [4字节长度] + [1字节标志] + [H264数据]
    unsigned int totalLen = sizeof(unsigned int) + 1 + nLen;
    std::vector<unsigned char> packet(totalLen);

    // 写入长度（网络字节序）
    unsigned int netLen = htonl(nLen + 1);
    memcpy(packet.data(), &netLen, sizeof(netLen));

    // 写入标志 (0x80 = 视频)
    packet[sizeof(netLen)] = 0x80;

    // 写入数据
    memcpy(packet.data() + sizeof(netLen) + 1, pData, nLen);

    // 发送给所有客户端
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    for (int fd : m_clientFds) {
        auto client = m_clients[fd];
        if (client && client->state == WS_OPEN) {
            SendWSFrame(fd, packet.data(), packet.size(), WS_BINARY);
        }
    }

    return 0;
}

int C_WebSocketServer::SendOpus(unsigned char* pData, unsigned int nLen)
{
    // 构建数据包: [4字节长度] + [1字节标志] + [Opus数据]
    unsigned int totalLen = sizeof(unsigned int) + 1 + nLen;
    std::vector<unsigned char> packet(totalLen);

    // 写入长度（网络字节序）
    unsigned int netLen = htonl(nLen + 1);
    memcpy(packet.data(), &netLen, sizeof(netLen));

    // 写入标志 (0x00 = 音频)
    packet[sizeof(netLen)] = 0x00;

    // 写入数据
    memcpy(packet.data() + sizeof(netLen) + 1, pData, nLen);

    // 发送给所有客户端
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    for (int fd : m_clientFds) {
        auto client = m_clients[fd];
        if (client && client->state == WS_OPEN) {
            SendWSFrame(fd, packet.data(), packet.size(), WS_BINARY);
        }
    }

    return 0;
}

void C_WebSocketServer::CloseClient(int fd)
{
    std::lock_guard<std::mutex> lock(m_clientsMutex);

    auto it = m_clients.find(fd);
    if (it != m_clients.end()) {
        // 发送关闭帧
        unsigned char closeFrame[] = {0x88, 0x00}; // FIN=1, opcode=CLOSE
        send(fd, closeFrame, sizeof(closeFrame), MSG_NOSIGNAL);

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
