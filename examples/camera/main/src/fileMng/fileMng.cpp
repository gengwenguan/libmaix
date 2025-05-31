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
#include "logAdapt.h"
#include "fileMng.h"
#include "logAdapt.h"
#include "h264Enc.h"

C_FileMng::C_FileMng(C_Listener* pListener)
    :m_pListrner(pListener),
    m_LastfileSize(0),
    m_bRunFlag(true),
    m_Thread( std::thread( [this]() { this->Accept(); }) )
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

    std::lock_guard<std::mutex> lock(m_fileMapMutex);
    struct dirent *entry = nullptr;
    while ((entry = readdir(dir)) != nullptr) // 遍历目录下所有文件
    {
        if (entry->d_type == DT_REG) // 如果是普通文件
        {
            std::string fileName = entry->d_name;
            std::string filePath = std::string(kFileDir) + fileName;

            unsigned int fileSize =  GetFileSize(filePath);

            if(fileSize < 10240){
                remove(filePath.c_str());  //删除较小的文件
            }else{
                //将I帧位置信息先从文件中读出来存入对应的map表中管理
                std::ifstream tmpFile(filePath.c_str(), std::ios::out | std::ios::binary);
                if(tmpFile.is_open()){
                    std::vector<long long> buffer(100, 0);
                    tmpFile.read((char *)buffer.data(), kMoovHeadLen);
                    tmpFile.close();

                    //判断I帧位置信息是否存在，不存在时该文件无法快进快退，拖动播放，对文件进行删除
                    if(std::all_of(buffer.begin(), buffer.end(), [](long long num) { return num == 0; })){
                        remove(filePath.c_str()); 
                    }else{
                        m_fileMap[filePath] = buffer; //只保留带有I帧位置信息头的文件
                    }
                }
            }
        }
    }
    closedir(dir); // 关闭目录

    m_outfilePath = GenerateFilePathByNowTime();
    CLOG_INF("filePath = %s!\n", m_outfilePath.c_str());

    std::lock_guard<std::mutex> locker(m_outFileMutex);
    //构造时先创建出来写入文件对象
    m_outFile.open(m_outfilePath.c_str(), std::ios::out | std::ios::binary);
    //使用vector创建并将I帧位置头初始化为全零
    std::vector<long long> buffer(100, 0);
    m_outFile.write((char*)buffer.data(), kMoovHeadLen);
    m_LastfileSize += kMoovHeadLen;
    //使用vector创建并将文件中I帧位置头初始化为全零
    m_fileMap[m_outfilePath] = buffer;

    //将新创建的文件放入管理map中
    //m_fileMap[filePath] = 0;
    //保证文件数量在最大限制之内,在没有回放客户端连接同时文件数量超过限制时才对过期文件进行删除
    while(m_fdConnections.size() == 0 && m_fileMap.size() > kMaxFileNum){
        std::string needRemoveFile = m_fileMap.begin()->first;
        remove(needRemoveFile.c_str());  //删除实际的文件
        m_fileMap.erase(needRemoveFile); //删除管理map中的文件
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

    if(m_Thread.joinable()){
        m_Thread.join();
    }
    std::lock_guard<std::mutex> lock(m_outFileMutex);
    if(m_outFile.is_open()){
        //均匀抽取100个I帧位置
        auto IdrPos = UniformResizeTo100(m_AllIdrPos);

        // for (size_t i = 0; i < IdrPos.size(); ++i) {
        //     CLOG_INF("IdrPos[%d] = %lld\n", i, IdrPos[i]);
        // }
        //将抽取的100个I帧位置写入文件头中
        m_outFile.seekp(0, std::ios::beg);
        m_outFile.write(reinterpret_cast<const char*>(IdrPos.data()), IdrPos.size() * sizeof(long long));
        m_outFile.close();
    }

    CLOG_INF("~C_FileMng end!\n");
}

//送入文件数据
void C_FileMng::InputFileData(unsigned char* data, unsigned int dataLen, char flag)
{
    std::lock_guard<std::mutex> lock(m_outFileMutex);
    if(m_outFile.is_open()){
        if(flag == 1){ //如果是视频文件将I帧在文件中的位置进行存储
            auto type = C_H264Enc::GetNALType(data, dataLen);
            if(type == C_H264Enc::NAL_SPS || type == C_H264Enc::NAL_IDR_PICTURE){

                m_AllIdrPos.push_back(m_LastfileSize.load());  
                //CLOG_INF("m_AllIdrPos.push_back! size=%d last=%lld\n", m_AllIdrPos.size(), m_LastfileSize.load());
            }
            // if(type == C_H264Enc::NAL_SPS){
            //     CLOG_INF("type == C_H264Enc::NAL_SPS!\n");
            // }
            // if(type == C_H264Enc::NAL_IDR_PICTURE){
            //     CLOG_INF("type == C_H264Enc::NAL_IDR_PICTURE!\n");
            // }
        }
        unsigned int allDataLen = dataLen+1;
        m_outFile.write((const char*)&allDataLen, sizeof(unsigned int));     //写入后面数据块长度
        m_outFile.write((const char*)&flag, 1);                              //写入一个字节标志位信息
        m_outFile.write((const char*)data, dataLen);                         //写入实际音视频负载数据
        m_LastfileSize += dataLen + sizeof(unsigned int) + 1;  //文件长度要统计到所有写入的数据
        //文件达到最大内存限制时进行关闭，重新创建一个新文件
        if(m_LastfileSize >= kMaxFileSize){
            std::lock_guard<std::mutex> lock(m_fileMapMutex);
            //均匀抽取100个I帧位置
            auto IdrPos = UniformResizeTo100(m_AllIdrPos);

            // for (size_t i = 0; i < IdrPos.size(); ++i) {
            //     CLOG_INF("IdrPos[%d] = %lld\n", i, IdrPos[i]);
            // }
            //将抽取的100个I帧位置写入文件头中
            m_outFile.seekp(0, std::ios::beg);
            m_outFile.write(reinterpret_cast<const char*>(IdrPos.data()), IdrPos.size() * sizeof(long long));
            //map中也存一份，I帧位置信息
            m_fileMap[m_outfilePath] = IdrPos;
            m_outFile.close();


            //生成新得到文件路径名
            std::string m_outfilePath = GenerateFilePathByNowTime();
            CLOG_INF("Generate new filePath = %s!\n", m_outfilePath.c_str());

            //新创建并打开一个文件
            m_outFile.open(m_outfilePath.c_str(), std::ios::out | std::ios::binary);
            //回调通知新文件创建
            m_pListrner->OnNewFileCreate();
            m_AllIdrPos.clear();
            m_LastfileSize = 0;

            //使用vector创建并将文件中I帧位置头初始化为全零
            std::vector<long long> buffer(100, 0);
            m_outFile.write((char*)buffer.data(), kMoovHeadLen);
            m_fileMap[m_outfilePath] = buffer;
            m_LastfileSize += kMoovHeadLen;
            
            //保证文件数量在最大限制之内,在没有回放客户端连接同时文件数量超过限制时才对过期文件进行删除
            while(m_fdConnections.size() == 0 && m_fileMap.size() > kMaxFileNum){
                std::string needRemoveFile = m_fileMap.begin()->first;
                remove(needRemoveFile.c_str());  //删除实际的文件
                m_fileMap.erase(needRemoveFile); //删除管理map中的文件
                CLOG_INF("needRemoveFile = %s\n", needRemoveFile.c_str());
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
                m_fdConnections[new_socket] = std::unique_ptr<C_ClientConnect>(new C_ClientConnect(this, new_socket));
            }
        }

        {
            std::lock_guard<std::mutex> lock(m_oMutex);
            //收到某条客户端连接发来的消息,目前回放控制消息长度为一个字节
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
long long C_FileMng::GetFileSize(std::string filePath)
{
    std::ifstream file(filePath, std::ios::binary);
    // 检查文件是否打开成功
    if (!file.is_open()) {
        std::cerr << "无法打开文件: " << filePath << std::endl;
        return 0;
    }
    // 获取文件大小
    file.seekg(0, std::ios::end);
    std::streampos fileSize = file.tellg();
    file.seekg(0, std::ios::beg);
    file.close();

    return fileSize;
}

std::vector<long long> C_FileMng::UniformResizeTo100(const std::vector<long long>& IdrPos) {
    const size_t targetSize = 100;
    std::vector<long long> result;
    
    if (IdrPos.empty()) {
        return std::vector<long long>(targetSize, 0);
    }
    
    result.reserve(targetSize);
    
    if (IdrPos.size() < targetSize) {
        // 扩展模式：使用最近邻插值
        for (size_t i = 0; i < targetSize; ++i) {
            // 计算在原始vector中的位置
            double pos = (double)i / (targetSize - 1) * (IdrPos.size() - 1);
            size_t idx = static_cast<size_t>(pos + 0.5); // 四舍五入到最近的索引
            
            // 确保不越界
            idx = std::min(idx, IdrPos.size() - 1);
            result.push_back(IdrPos[idx]);
        }
    } else if (IdrPos.size() > targetSize) {
        // 抽取模式：均匀选择现有值
        double step = static_cast<double>(IdrPos.size() - 1) / (targetSize - 1);
        
        for (size_t i = 0; i < targetSize; ++i) {
            double pos = i * step;
            size_t idx = static_cast<size_t>(pos + 0.5); // 四舍五入到最近的索引
            
            // 确保不越界
            idx = std::min(idx, IdrPos.size() - 1);
            result.push_back(IdrPos[idx]);
        }
    } else {
        // 正好100个元素
        return IdrPos;
    }
    
    return result;
}