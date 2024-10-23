#include "tcpserver.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <errno.h>

constexpr int PORT = 56050;
C_TcpServer::C_TcpServer(C_Listener* pListrner)
    :m_pListrner(pListrner)
{
    m_bRunFlag = true;
    m_pThread = new std::thread([this]() {
                this->Accept();
            });
}

C_TcpServer::~C_TcpServer()
{
    m_bRunFlag = false;
    //close(m_server_fd);
    shutdown(m_server_fd, SHUT_RDWR);  // 关闭监听套接字
    if(m_pThread != nullptr){
        m_pThread->join();
        delete m_pThread;
    }

    std::lock_guard<std::mutex> lock(m_oMutex);
    for(auto it = m_fdSet.begin(); it != m_fdSet.end();){
        close(*it);
    }
    m_fdSet.clear();
}

//下放H264数据给所有连接的客户端
int C_TcpServer::SendH264(unsigned char* pData, unsigned int nLen)
{
    int ret = 0;
    std::lock_guard<std::mutex> lock(m_oMutex);
    for(auto it = m_fdSet.begin(); it != m_fdSet.end();){
        // 转换整数的字节序为网络字节序
        int networkNumber = htonl(nLen);
        //先将一帧H264数据的长度发送给客户端，长度为4个字节
        ret = send(*it, &networkNumber, sizeof(networkNumber), 0);  
        if(ret>0){
            //再将实际的H264数据发送给客户端
            ret = send(*it, pData, nLen, 0);                         
        }

        if (ret <= 0) {
            // 发送失败关闭socket
            printf("send failed, socket:%d closed\n", *it);
            close(*it);
            // 使用 erase 方法来删除元素，并更新迭代器
            it = m_fdSet.erase(it); 
        } else {
            ++it; // 只有在没有删除时，才移动到下一个元素
        }
    }
    
    return 0;
}

int C_TcpServer::Accept(){

    int new_socket;
    struct sockaddr_in address;
    int addrlen = sizeof(address);

    // 创建 socket 文件描述符
    if ((m_server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    // 配置服务器地址信息
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    // 绑定 socket 到地址
    if (bind(m_server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind failed");
        close(m_server_fd);
        exit(EXIT_FAILURE);
    }

    // 监听连接
    if (listen(m_server_fd, 3) < 0) {
        perror("listen failed");
        close(m_server_fd);
        exit(EXIT_FAILURE);
    }

    printf("Server listening on port %d\n", PORT);

    while(m_bRunFlag){
        // 接受连接
        if ((new_socket = accept(m_server_fd, (struct sockaddr *)&address, (socklen_t*)&addrlen)) < 0) {
            perror("accept failed");
        }else{
            std::lock_guard<std::mutex> lock(m_oMutex);
            printf("accept new socket:%d\n", new_socket);
            m_fdSet.insert(new_socket);
            //通知监听器有新的客户端连接
            m_pListrner->OnNewClientConnect(new_socket);
        }
    }

    close(m_server_fd);
    printf("End accepted\n");

    return 0;
}