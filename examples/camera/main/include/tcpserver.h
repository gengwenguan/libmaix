#pragma once
#include <iostream>
#include <thread>
#include <mutex>
#include <set>

class C_TcpServer
{
public:
    class C_Listener
        {
        public:
            virtual ~C_Listener() = default;
            /*新客户端连接事件*/
            virtual int OnNewClientConnect(int fd) = 0;
        };
public:
    C_TcpServer(C_Listener* pListrner);
    ~C_TcpServer();

    //下放H264数据,发送给每个连接的客户端
    int SendH264(unsigned char* pData, unsigned int nLen);

private:
    //接收客户端连接
    int Accept();

private:
    C_Listener*  m_pListrner;  //监听器
    std::thread* m_pThread;    //接收客户端连接线程
    bool         m_bRunFlag;   //线程运行标识
    int          m_server_fd;


    std::mutex    m_oMutex;    //互斥锁
    std::set<int> m_fdSet;     //客户端连接集合
};