#include<vector>
#include <arpa/inet.h>
#include <errno.h>
#include <algorithm>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include"h264Enc.h"
#include"clientConnect.h"


C_ClientConnect::C_ClientConnect(C_Listener* pListener,int socketFd)
    :m_pListener(pListener),
    m_sockeFd(socketFd),
    m_progress(0),
    m_IntervalMs(kIntervalDefaultMs)
{
    //设置日志输出的key，用于区分多个连接产生的打印
    GetLogKey() << "socketFd:" << m_sockeFd;
    std::lock_guard<std::mutex> lock(m_pListener->GetfileMapMutex());
    m_fileMapIter = pListener->GetfileMap().begin();
    m_sendFile.open(m_fileMapIter->first, std::ios::binary);
    NLOG_INF("m_fileMapIter->first.c_str()=%s  m_sendFile.is_open()=%d\n", m_fileMapIter->first.c_str(), m_sendFile.is_open());

    //先读取文件中存储的I帧位置
    m_sendFile.read(reinterpret_cast<char*>(m_IdrPos100.data()), m_IdrPos100.size() * sizeof(long long));
    for (size_t i = 0; i < m_IdrPos100.size(); ++i) {
        NLOG_INF("m_IdrPos100[%d] = %lld\n", i, m_IdrPos100[i]);
    }
    //判断是否读到了I帧位置信息，没读到时间进行临时获取
    if(std::all_of(m_IdrPos100.begin(), m_IdrPos100.end(), [](long long num) { return num == 0; })){
        auto it = m_fileMapIter; 
        ++it;   //判断当前迭代器是不是最后一个map元素
        if(it == m_pListener->GetfileMap().end()){
            //还是最后一个正在写的文件时临时获取
            m_IdrPos100 = m_pListener->GetTmpIdrPos100();
        }else{ 
            //文件已经写完毕落盘了，则直接获取则可
            m_IdrPos100 = m_fileMapIter->second;
        }
    }
    if(m_IdrPos100.size() == 0)
        NLOG_ERR("m_IdrPos100.size() == 0");
    for (size_t i = 0; i < m_IdrPos100.size(); ++i) {
        NLOG_INF("after m_IdrPos100[%d] = %lld\n", i, m_IdrPos100[i]);
    }
    m_bRunFlag = true;
    m_Thread = std::thread( [this]() { this->SendFileTask(); });

}

C_ClientConnect::~C_ClientConnect()
{
    m_bRunFlag = false;
    if(m_Thread.joinable()){
        m_Thread.join();
    }

    if(m_sendFile.is_open()){
        m_sendFile.close();
    }
    NLOG_INF("this=%p \n", this);
}

//接收到取流端发来的控制消息：进度条拖动、快进、快退、上一个文件、下一个文件
int C_ClientConnect::RecvCtrlMesssage(char* pData, unsigned int nLen)
{
    //仅只支持控制消息长度为一
    if(nLen != 1){
        NLOG_ERR("only support nLen == 1, nLen(%d)\n", nLen);
        return -1;
    }
    unsigned char message = pData[0];
    NLOG_INF("recv message(%d)\n", message);
    std::lock_guard<std::mutex> lock(m_MessageMutex);
    m_MessageList.push_back(message);
    return 0;
}

//处理接收到的控制消息
int C_ClientConnect::HandleCtrlMesssage(){
    std::list<unsigned char> tmpMessageList;
    {
        std::lock_guard<std::mutex> lock(m_MessageMutex);
        tmpMessageList = std::move(m_MessageList);
        m_MessageList.clear();
    }

    for(auto message : tmpMessageList){
        //0~100区间的消息为进度条进度,调整播放位置
        if(message >= 0 && message < 100){

            if(m_IdrPos100[message] >= 100*sizeof(long long)){
                m_sendFile.seekg(m_IdrPos100[message], std::ios::beg);
                //偏移到指定位置
                NLOG_INF("seek to <%d>  %d %!\n", m_IdrPos100[message], message);
            }else{
                NLOG_ERR("m_IdrPos100[%d]<%d> invied!\n", message, m_IdrPos100[message]);
            }

        }else if(message == 101){ // 101为快退
            if(m_progress>1){
                //需要减2，否则一直退不到后面
                m_sendFile.seekg(m_IdrPos100[m_progress-2], std::ios::beg);
                NLOG_INF("Fast back <%d>!\n", m_IdrPos100[m_progress-1]);
            }else{
                NLOG_ERR("Fast back invied! m_progress<%d>\n", m_progress);
            }
        }else if(message == 102){ // 102为快进
            if(m_progress<99){
                m_sendFile.seekg(m_IdrPos100[m_progress+1], std::ios::beg);
                NLOG_INF("Fast forward <%d>!\n", m_IdrPos100[m_progress-1]);
            }else{
                NLOG_ERR("Fast forward invied! m_progress<%d>\n", m_progress);
            }
        }else if(message == 104){ // 104为上一个文件
            if(m_sendFile){
                std::lock_guard<std::mutex> lock(m_pListener->GetfileMapMutex());
                if(m_fileMapIter == m_pListener->GetfileMap().begin()){
                    NLOG_ERR("m_fileMapIter == m_fileMap.begin()!, cannot jump to the previous file!\n");
                    return -1;
                }
                m_sendFile.close();
                --m_fileMapIter;
                m_sendFile.open(m_fileMapIter->first.c_str(), std::ios::binary);
                //先读取文件中存储的I帧位置
                m_sendFile.read(reinterpret_cast<char*>(m_IdrPos100.data()), m_IdrPos100.size() * sizeof(long long));
                //判断是否读到了I帧位置信息，没读到时间进行临时获取
                if(std::all_of(m_IdrPos100.begin(), m_IdrPos100.end(), [](long long num) { return num == 0; })){
                    auto it = m_fileMapIter; 
                    ++it;   //判断当前迭代器是不是最后一个map元素
                    if(it == m_pListener->GetfileMap().end()){
                        //还是最后一个正在写的文件时临时获取
                        m_IdrPos100 = m_pListener->GetTmpIdrPos100();
                    }else{ 
                        //文件已经写完毕落盘了，则直接获取则可
                        m_IdrPos100 = m_fileMapIter->second;
                    }
                }
                NLOG_INF("jump to the previous file<%s>!\n", m_fileMapIter->first.c_str());
            }else{
                NLOG_ERR("jump to the previous file m_sendFile == NULL!\n");
            }
        }else if(message == 105){ // 105为下一个文件
            if(m_sendFile){
                std::lock_guard<std::mutex> lock(m_pListener->GetfileMapMutex());
                if(++m_fileMapIter == m_pListener->GetfileMap().end()){
                    NLOG_ERR("m_fileMapIter next is m_fileMap.end(), This time the control failed!\n");
                    m_fileMapIter--;
                    return -1;
                }
                m_sendFile.close();
                m_sendFile.open(m_fileMapIter->first.c_str(), std::ios::binary);
                //先读取文件中存储的I帧位置
                m_sendFile.read(reinterpret_cast<char*>(m_IdrPos100.data()), m_IdrPos100.size() * sizeof(long long));
                //判断是否读到了I帧位置信息，没读到时间进行临时获取
                if(std::all_of(m_IdrPos100.begin(), m_IdrPos100.end(), [](long long num) { return num == 0; })){
                    auto it = m_fileMapIter; 
                    ++it;   //判断当前迭代器是不是最后一个map元素
                    if(it == m_pListener->GetfileMap().end()){
                        //还是最后一个正在写的文件时临时获取
                        m_IdrPos100 = m_pListener->GetTmpIdrPos100();
                    }else{ 
                        //文件已经写完毕落盘了，则直接获取则可
                        m_IdrPos100 = m_fileMapIter->second;
                    }
                }
                NLOG_INF("jump to the next file<%s>!\n", m_fileMapIter->first.c_str());
            }else{
                NLOG_ERR("jump to the next file m_sendFile == NULL!\n");
            }
        }else if(message == 107){ // 107为视频播放加速
            m_IntervalMs = kIntervalFastPlayMs;
            NLOG_INF("Fast Forword start m_IntervalMs<%d>!\n", m_IntervalMs);
        }else if(message == 108){ // 108为停止视频播放加速
            m_IntervalMs = kIntervalDefaultMs;
            NLOG_INF("Fast Forword stop m_IntervalMs<%d>!\n", m_IntervalMs);
        }else{
            NLOG_ERR("Unsupport message(%d)\n", message);
            return -1;
        }
    }

    return 0;
}
    

//向客户端通过网络发送Chuck数据
int C_ClientConnect::SendChuck(char* pData, unsigned int nLen)
{
    // 转换整数的字节序为网络字节序
    int networkNumber = htonl(nLen);
    //先将Chuck数据的长度发送给客户端，长度为4个字节
    int ret = send(m_sockeFd, &networkNumber, sizeof(networkNumber), MSG_NOSIGNAL);
    if(ret>0){
        //再将实际的Chuck数据发送给客户端
        ret = send(m_sockeFd, pData, nLen, MSG_NOSIGNAL);
    }

    if (ret <= 0) {
        // 发送失败
        NLOG_ERR("send failed, socket:%d\n", m_sockeFd);
    }
    return ret;
}

void C_ClientConnect::SendFileTask()
{   
    NLOG_INF("SendFileTask m_sockeFd=%d\n", m_sockeFd);     
    while(m_bRunFlag){
        //先处理远端发来的控制信令
        HandleCtrlMesssage();

        unsigned int allDataLen = 0;
        
        //先读取数据长度
        m_sendFile.read((char*)&allDataLen, sizeof(allDataLen));
        if(CheckSendFileEof()) continue;

        //从文件中读取数据的缓冲区
        std::vector<char> buffer(allDataLen);
        m_sendFile.read(buffer.data(), allDataLen);
        if(CheckSendFileEof()) continue;

        
        //提取flag判断是音频还是视频
        char flag = buffer[0];

        //NLOG_INF("SendFileTask m_sockeFd=%d allDataLen=%d flag=%d\n", m_sockeFd, allDataLen, flag);   

        // 计算视频播放的进度
        std::streampos currentPos = m_sendFile.tellg();
        auto it = std::lower_bound(m_IdrPos100.begin(), m_IdrPos100.end(), static_cast<long long>(currentPos));
        m_progress = it - m_IdrPos100.begin();

        // 将音频还是视频的标记信息放在最高位，低7位存放视频播放的进度信息
        char writeflag = flag << 7 | m_progress;
        //设置好的标志位再放入待发送缓冲区中
        buffer[0] = writeflag;

        SendChuck(buffer.data(), buffer.size());

        //发送一帧视频数据后进行相应延时
        if(flag == 1){
            std::this_thread::sleep_for(std::chrono::milliseconds(m_IntervalMs));
        }
    }
}

//检查是否到达文件结尾，如果到达则打开新的文件
bool C_ClientConnect::CheckSendFileEof(){
    if(m_sendFile.eof()){
        m_sendFile.close();  //某个文件读取到末尾了关闭该文件，打开下一个文件，让客户端依次播放录制的视频文件
        
        std::lock_guard<std::mutex> lock(m_pListener->GetfileMapMutex());
        //最后一个文件也播放完毕时重新播放该文件
        if(++m_fileMapIter == m_pListener->GetfileMap().end()){
            NLOG_WRN("Replay last file!\n");
            --m_fileMapIter; // = m_pListener->GetfileMap().begin();
        }
        
        m_sendFile.open(m_fileMapIter->first, std::ios::binary);
        //先读取文件中存储的I帧位置
        m_sendFile.read(reinterpret_cast<char*>(m_IdrPos100.data()), m_IdrPos100.size() * sizeof(long long));
        //判断是否读到了I帧位置信息，没读到是进行临时获取
        if(std::all_of(m_IdrPos100.begin(), m_IdrPos100.end(), [](long long num) { return num == 0; })){
            auto it = m_fileMapIter; 
            ++it;   //判断当前迭代器是不是最后一个map元素
            if(it == m_pListener->GetfileMap().end()){
                //还是最后一个正在写的文件时临时获取
                m_IdrPos100 = m_pListener->GetTmpIdrPos100();
            }else{ 
                //文件已经写完毕落盘了，则直接获取则可
                m_IdrPos100 = m_fileMapIter->second;
            }
        }
        NLOG_INF("Play the next file<%s>!\n", m_fileMapIter->first.c_str());
        return true;
    }
    return false;
}

//最新生成的文件实时获取
// void C_ClientConnect::GetIdrPos100(){
//     std::lock_guard<std::mutex> lock(m_pListener->GetfileMapMutex());
//     auto it = m_fileMapIter; 
//     ++it;   //判断当前迭代器是不是最后一个map元素
//     if(it == m_pListener->GetfileMap().end()){
//         //还是最后一个正在写的文件时临时获取
//         m_IdrPos100 = m_pListener->GetTmpIdrPos100();
//     }else{ 
//         //文件已经写完毕落盘了，则直接获取则可
//         m_IdrPos100 = m_fileMapIter->second;
//     }
// }