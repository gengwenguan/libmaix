#pragma once
#include <iostream>
#include <thread>
#include <mutex>
#include <set>
#include <memory>
#include <string>
#include <vector>
#include <functional>

class C_HttpServer
{
public:
    C_HttpServer(int port);
    ~C_HttpServer();

    int Start();
    void Stop();

    int GetClientCount();

private:
    void AcceptThread();
    void ProcessClient(int fd);
    void HandleHttpRequest(int fd, const std::string& request);
    void SendHttpResponse(int fd, int statusCode, const std::string& statusText, 
                          const std::string& contentType, const std::string& content);

private:
    int m_port;
    int m_server_fd;
    bool m_bRunFlag;
    std::thread m_acceptThread;
    std::mutex m_clientsMutex;
    std::set<int> m_clientFds;
};
