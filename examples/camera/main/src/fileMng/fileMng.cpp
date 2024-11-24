#include <dirent.h>
#include <sys/stat.h>
#include <vector>
#include <algorithm>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <errno.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include"logAdapt.h"
#include"fileMng.h"
#include"logAdapt.h"

C_FileMng::C_FileMng()
    :m_bRunFlag(true),
    m_pThread( new std::thread( [this]() { this->Accept(); }) )
{
    //要先确保存放视频的文件夹存在
    DIR *dir = opendir(kFileDir); // 打开目录
    if (dir == nullptr)
    {
        mkdir(kFileDir, 0775); // 打开失败时可能是由于目录不存在，尝试主动创建
        CLOG_ERR("Directory don`t exit create it now --- %s\n", kFileDir);
        dir = opendir(kFileDir); // 打开目录
        if (dir == nullptr)
        {
            CLOG_ERR("Failed to open %s\n", kFileDir);
            return;
        }
    }

    struct dirent *entry = nullptr;
    while ((entry = readdir(dir)) != nullptr) // 遍历目录下所有文件
    {
        if (entry->d_type == DT_REG) // 如果是普通文件
        {
            std::string fileName = entry->d_name;
            std::string filePath = std::string(kFileDir) + fileName;

            //将目录下原本就存在的文件大小信息放入文件map中
            std::lock_guard<std::mutex> lock(m_fileMapMutex);
            m_fileMap[filePath] = GetFileSize(filePath);
        }
    }
    closedir(dir); // 关闭目录

    std::string filePath = GenerateFilePathByNowTime();
    CLOG_INF("filePath = %s!\n", filePath.c_str());

    //构造时先创建出来写入文件对象
    m_outFile.open(filePath.c_str(), std::ios::out | std::ios::binary);

    {
        //将新创建的文件放入管理map中
        std::lock_guard<std::mutex> lock(m_fileMapMutex);
        m_fileMap[filePath] = 0;
        //保证文件数量在最大限制之内,在没有回放客户端连接同时文件数量超过限制时才对过期文件进行删除
        while(m_fdConnections.size() == 0 && m_fileMap.size() > kMaxFileNum){
            std::string needRemoveFile = m_fileMap.begin()->first;
            remove(needRemoveFile.c_str());  //删除实际的文件
            m_fileMap.erase(needRemoveFile); //删除管理map中的文件
        }
    }

    CLOG_INF("m_outFile.open =  %d!\n", m_outFile.is_open());


}

C_FileMng::~C_FileMng()
{
    CLOG_INF("~C_FileMng enter!\n");
    {
        std::lock_guard<std::mutex> lock(m_oMutex);
        m_fdConnections.clear();
    }    
    m_bRunFlag = false;
    if(m_pThread != nullptr){
        m_pThread->join();
    }
    if(m_outFile.is_open()){
        m_outFile.close();
    }

    CLOG_INF("~C_FileMng end!\n");
}

//送入文件数据
void C_FileMng::InputFileData(unsigned char* data, unsigned int dataLen)
{
    if(m_outFile.is_open()){
        m_outFile.write((const char*)data, dataLen);
        m_fileSize += dataLen;
        //文件达到最大内存限制时进行关闭，重新创建一个新文件
        if(m_fileSize >= kMaxFileSize){
            m_outFile.close();
            m_fileSize = 0;
            {
                std::lock_guard<std::mutex> lock(m_fileMapMutex);
                //更新最后一个文件大小
                if(m_fileMap.size() > 0){
                    std::string needRefreshFile = m_fileMap.rbegin()->first;
                    m_fileMap[needRefreshFile] = GetFileSize(needRefreshFile);
                }
            }

            //生成新得到文件路径名
            std::string filePath = GenerateFilePathByNowTime();
            CLOG_INF("filePath = %s!\n", filePath.c_str());

            //新创建并打开一个文件
            m_outFile.open(filePath.c_str(), std::ios::out | std::ios::binary);

            {
                //将新创建的文件放入管理map中
                std::lock_guard<std::mutex> lock(m_fileMapMutex);
                m_fileMap[filePath] = 0;
                //保证文件数量在最大限制之内,在没有回放客户端连接同时文件数量超过限制时才对过期文件进行删除
                while(m_fdConnections.size() == 0 && m_fileMap.size() > kMaxFileNum){
                    std::string needRemoveFile = m_fileMap.begin()->first;
                    remove(needRemoveFile.c_str());  //删除实际的文件
                    m_fileMap.erase(needRemoveFile); //删除管理map中的文件
                }
            }

        }
    }else{
        CLOG_ERR("m_outFile not open!\n");
    }
}

int C_FileMng::Accept(){
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
    address.sin_port = htons(kFileMngPort);

    // 绑定 socket 到地址
    if (bind(m_server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        CLOG_ERR("bind failed\n");
        close(m_server_fd);
        exit(EXIT_FAILURE);
    }

    // 监听连接
    if (listen(m_server_fd, 5) < 0) {
        CLOG_ERR("listen failed\n");
        close(m_server_fd);
        exit(EXIT_FAILURE);
    }

    CLOG_INF("Server listening on port %d\n", kFileMngPort);

    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(m_server_fd, &read_fds);
    while(m_bRunFlag){
        fd_set tmp_fds = read_fds;
        int max_fd = m_server_fd;
        // 将所有客户端套接字添加到文件描述符集
        {
            std::lock_guard<std::mutex> lock(m_oMutex);
            for (auto& item : m_fdConnections) {
                int fd = item.first;
                if (fd > max_fd) {
                    max_fd = fd;
                }
            }
        }

        timeval tm{};
        tm.tv_sec = 1;
        tm.tv_usec =0;
        int ret = select(max_fd + 1, &tmp_fds, NULL, NULL, &tm);
        if (ret < 0) {
            CLOG_ERR("select ret=%d\n", ret);
        } else if (ret == 0) { //超时
            continue;
        }

        //服务器接收到新的连接
        if (FD_ISSET(m_server_fd, &tmp_fds)) {
            int new_socket;
            // 接收连接
            if ((new_socket = accept(m_server_fd, (struct sockaddr *)&address, (socklen_t*)&addrlen)) < 0) {
                CLOG_ERR("accept failed\n");
            }else{
                std::lock_guard<std::mutex> lock(m_oMutex);
                CLOG_INF("accept new socket:%d\n", new_socket);
                FD_SET(new_socket, &read_fds);
                m_fdConnections[new_socket] = std::unique_ptr<C_ClientConnect>(new C_ClientConnect(new_socket, m_fileMap, m_fileMapMutex));
            }
        }

        {
            std::lock_guard<std::mutex> lock(m_oMutex);
            //收到某条客户端连接发来的消息,目前预览端的控制消息长度为一个字节
            char buffer[1];
            for(auto iter = m_fdConnections.begin(); iter != m_fdConnections.end(); ){
                int fd = iter->first;
                if(FD_ISSET(fd, &tmp_fds)){
                    int ret = recv(fd, buffer, 1, MSG_NOSIGNAL);
                    if (ret <= 0) {
                        // 客户端断开连接
                        close(fd);
                        FD_CLR(fd, &read_fds);
                        iter = m_fdConnections.erase(iter);
                        CLOG_INF("Close socket:%d m_fdConnections.size()=%d\n", fd, m_fdConnections.size());
                        continue;
                    } else {
                        // 处理接收到的消息
                        iter->second->RecvCtrlMesssage(buffer, ret);
                    }
                }
                iter++; //迭代器递增放在尾部，以便上面的continue语句可以跳过
            }
        }

    }

    CLOG_INF("End accepted\n");

    return 0;
}

//根据当前时间生成文件名
std::string C_FileMng::GenerateFilePathByNowTime()
{
    time_t timep;
    struct tm *p;
    char path[256] = {0};
    time(&timep);                                                                                                           // 获取从1970至今过了多少秒，存入time_t类型的timep
    p = localtime(&timep);
    p->tm_hour += 8;
    timep = mktime(p);
    p = localtime(&timep);
    sprintf(path, "%s%d_%02d_%02d_%02d-%02d-%02d.264", kFileDir, 1900 + p->tm_year, 1 + p->tm_mon, p->tm_mday, p->tm_hour, p->tm_min, p->tm_sec); // 把格式化的时间写入字符数组中

    return std::string(path);
}

//获取文件大小
unsigned int C_FileMng::GetFileSize(std::string filePath)
{
    std::ifstream file(filePath, std::ios::binary);
    // 获取文件大小
    file.seekg(0, std::ios::end);
    std::streampos fileSize = file.tellg();
    file.seekg(0, std::ios::beg);
    file.close();

    return fileSize;
}