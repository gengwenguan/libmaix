#include "tcpServer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <errno.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include"logAdapt.h"

C_TcpServer::C_TcpServer(C_Listener* pListrner)
    :m_pListrner(pListrner),
    m_bRunFlag(true),
    m_Thread( std::thread( [this]() { this->Accept(); }) )
{
}

C_TcpServer::~C_TcpServer()
{
    m_bRunFlag = false;
    if(m_Thread.joinable()){
        m_Thread.join();
    }
    
    if(m_server_fd>0){
        std::lock_guard<std::mutex> lock(m_oMutex);
        for(auto it = m_fdSet.begin(); it != m_fdSet.end(); ++it){
            close(*it);
        }
        CLOG_INF("m_fdSet.clear();\n");
        m_fdSet.clear();
        //shutdown(m_server_fd, SHUT_RDWR);  // 关闭监听套接字
    }
    close(m_server_fd);

    CLOG_INF("~C_TcpServer()\n");
}


//下放媒体数据,发送给每个连接的客户端 flag = 0-音频 1-视频
int C_TcpServer::SendMedia(unsigned char* pData, unsigned int nLen, char flag){
    int ret = 0;
    std::lock_guard<std::mutex> lock(m_oMutex);
    for(auto it = m_fdSet.begin(); it != m_fdSet.end();){
        // 转换整数的字节序为网络字节序
        int networkNumber = htonl(nLen+1);
        //先将一帧H264数据的长度发送给客户端，长度为4个字节
        ret = send(*it, &networkNumber, sizeof(networkNumber), MSG_NOSIGNAL);
        if(ret>0){
            send(*it, &flag, 1, MSG_NOSIGNAL);
            //再将实际的H264数据发送给客户端
            ret = send(*it, pData, nLen, MSG_NOSIGNAL);
        }

        if (ret <= 0) {
            // 发送失败关闭socket
            CLOG_INF("send failed, socket:%d closed\n", *it);
            close(*it);
            // 使用 erase 方法来删除元素，并更新迭代器
            it = m_fdSet.erase(it); 
        } else {
            ++it; // 只有在没有删除时，才移动到下一个元素
        }
    }
    return 0;
}

//下放H264数据给所有连接的客户端
int C_TcpServer::SendH264(unsigned char* pData, unsigned int nLen){
    return SendMedia(pData, nLen, 1);
}

//下放opus数据,发送给每个连接的客户端
int C_TcpServer::SendOpus(unsigned char* pData, unsigned int nLen){
    return SendMedia(pData, nLen, 0);
}

int C_TcpServer::Accept(){
    // 创建 socket 文件描述符
    if ((m_server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        CLOG_ERR("socket failed\n");
        exit(EXIT_FAILURE);
    }

    // 配置服务器地址信息
    struct sockaddr_in address;
    int addrlen = sizeof(address);
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    // 绑定 socket 到地址
    if (bind(m_server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        CLOG_ERR("bind failed\n");
        close(m_server_fd);
        exit(EXIT_FAILURE);
    }

    // 监听连接
    if (listen(m_server_fd, 3) < 0) {
        CLOG_ERR("listen failed\n");
        close(m_server_fd);
        exit(EXIT_FAILURE);
    }

    CLOG_INF("Server listening on port %d\n", PORT);

    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(m_server_fd, &read_fds);
    timeval tm{};
    tm.tv_sec = 1;
    tm.tv_usec =0;
    while(m_bRunFlag){
        fd_set tmp_fds = read_fds;
        int ret = select(m_server_fd + 1, &tmp_fds, NULL, NULL, &tm);
        if (ret < 0) {
            CLOG_ERR("select ret=%d\n", ret);
        } else if (ret == 0) { //超时
            continue;
        }

        if (FD_ISSET(m_server_fd, &tmp_fds)) {
            int new_socket;
            // 接受连接
            if ((new_socket = accept(m_server_fd, (struct sockaddr *)&address, (socklen_t*)&addrlen)) < 0) {
                CLOG_ERR("accept failed\n");
            }else{
                std::lock_guard<std::mutex> lock(m_oMutex);
                CLOG_INF("accept new socket:%d\n", new_socket);
                m_fdSet.insert(new_socket);
                //通知监听器有新的客户端连接
                m_pListrner->OnNewClientConnect(new_socket);
            }
        }
    }

    //shutdown(m_server_fd, SHUT_RDWR);

    CLOG_INF("End accepted\n");

    return 0;
}