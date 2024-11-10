/********************************************************************************* 
  *Copyright(C),Your Company 
  *FileName:  tcpserver.h
  *Author:    gengwenguan
  *Date:      2024-10-25
  *Description:  用于接收客户端连接，默认监听端口56050
                 提供发送H264码流的能力，发送实际数据之前会先发送四字节
                 实际数据长度，客户端要先接收长度，再接收实际数据
                 该类提供新客户端连接通知，可以根据通知编码关键帧让客户端立马出图
**********************************************************************************/ 
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