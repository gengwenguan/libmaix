#include<vector>
#include <arpa/inet.h>
#include <errno.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include"h264Enc.h"
#include"clientConnect.h"


C_ClientConnect::C_ClientConnect(C_Listener* pListener,int socketFd)
    :m_pListener(pListener),
    m_sockeFd(socketFd),
    m_progress(0),
    m_bNeedIframe(false),
    m_IntervalMs(kIntervalDefaultMs)
{
    //设置日志输出的key，用于区分多个连接产生的打印
    GetLogKey() << "socketFd:" << m_sockeFd;
    std::lock_guard<std::mutex> lock(m_pListener->GetfileMapMutex());
    m_fileMapIter = pListener->GetfileMap().begin();
    m_sendFile.open(m_fileMapIter->first, std::ios::binary);
    NLOG_INF("m_fileMapIter->first.c_str()=%s  m_sendFile.is_open()=%d\n", m_fileMapIter->first.c_str(), m_sendFile.is_open());

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
        if(message >= 0 && message <= 100){
            m_bNeedIframe = true;
            //要调整的偏移位置
            int offset = GetSendFileSize() / 100 * message;
            //偏移到指定位置
            m_sendFile.seekg(offset, std::ios::beg);
            NLOG_INF("seek to <%d>!\n", offset);

        }else if(message == 101){ // 101为快退
            m_bNeedIframe = true;
            //要调整的偏移位置
            int offset = GetSendFileSize() / kJumpPercentage;
            //防止偏移越界
            std::streampos currentPos = m_sendFile.tellg();
            if(offset > currentPos){
                m_sendFile.seekg(0, std::ios::beg); //偏移超过文件开头时限制在开头
                NLOG_INF("Fast back to head offset<%d>!\n", offset);
            }else{
                m_sendFile.seekg(-offset, std::ios::cur);
                NLOG_INF("Fast back offset<%d>!\n", offset);
            }
        }else if(message == 102){ // 102为快进
            m_bNeedIframe = true;
            long long SendFileSize = GetSendFileSize();
            //要调整的偏移位置
            int offset = SendFileSize / kJumpPercentage;
            //防止偏移越界
            std::streampos currentPos = m_sendFile.tellg();
            if(offset + currentPos > SendFileSize){
                m_sendFile.seekg(0, std::ios::end); //偏移超过文件开头时限制在开头
                NLOG_INF("Fast forward to file end!\n");
            }else{
                m_sendFile.seekg(offset, std::ios::cur);
                NLOG_INF("Fast forward offset<%d> SendFileSize<%d>!\n", offset, SendFileSize);
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
    

//向客户端通过网络发送NAL数据
int C_ClientConnect::SendNal(char* pData, unsigned int nLen)
{
    // 转换整数的字节序为网络字节序,长度加一，前方增加一个字节视频播放的进度信息
    int networkNumber = htonl(nLen+1);
    //先将一帧H264数据的长度发送给客户端，长度为4个字节
    int ret = send(m_sockeFd, &networkNumber, sizeof(networkNumber), MSG_NOSIGNAL);
    if(ret>0){
        //先将一字节视频播放进度信息发送给客户端
        char byte = (char)m_progress;
        send(m_sockeFd, &byte, 1, MSG_NOSIGNAL);
        //再将实际的H264数据发送给客户端
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
    //return;
    // 查找 NAL 起始码的 lambda 表达式
    auto findNALStart = [](char* buffer, size_t bufLen, size_t& startPos) -> bool {
        for (size_t i = 0; i < bufLen - 3; ++i) {
            // 检测4字节或3字节起始码
            if (buffer[i] == 0x00 && buffer[i + 1] == 0x00 && 
                (buffer[i + 2] == 0x01 || (buffer[i + 2] == 0x00 && buffer[i + 3] == 0x01))) {
                startPos = i;
                return true;
            }
        }
        return false;
    };

    //存放完整NAL单元缓冲区
    std::vector<char> nalBuffer;             
    while(m_bRunFlag){

        //先处理远端发来的控制信令
        HandleCtrlMesssage();

        //从文件中读取数据的缓冲区
        std::vector<char> buffer(kTmpBuffSize);
   
        if(m_sendFile.eof()){
            m_sendFile.close();  //某个文件读取到末尾了关闭该文件，打开下一个文件，让客户端依次播放录制的视频文件
            {
                std::lock_guard<std::mutex> lock(m_pListener->GetfileMapMutex());
                //最后一个文件也播放完毕时重新播放该文件
                if(++m_fileMapIter == m_pListener->GetfileMap().end()){
                    NLOG_WRN("Replay last file!\n");
                    --m_fileMapIter; // = m_pListener->GetfileMap().begin();
                }
            }
            m_sendFile.open(m_fileMapIter->first, std::ios::binary);
            NLOG_INF("Play the next file<%s>!\n", m_fileMapIter->first.c_str());
        }

        
         //NLOG_INF("m_progress = %d!\n", m_progress);

        //文件中读取部分数据
        m_sendFile.read(buffer.data(), kTmpBuffSize);
        std::streamsize bytesRead = m_sendFile.gcount();

        //计算文件播放进度
        std::streampos currentPos = m_sendFile.tellg();
        m_progress = (int)((double)currentPos / GetSendFileSize() * 100);
        m_progress = std::min(m_progress, 100);

        if (bytesRead > 0) {
            //调整缓冲区为实际读取数据大小
            buffer.resize(bytesRead);

            size_t readPos = 0;//读取的位置
            while(readPos < (size_t)bytesRead){
                size_t startPos = 0;

                if(findNALStart(buffer.data()+readPos, buffer.size()-readPos, startPos)){
                    nalBuffer.insert(nalBuffer.end(), buffer.begin()+readPos , buffer.begin()+readPos+startPos);
                    //找到了下一个NALU单元的起始码，同时nalBuffer中有数据时说明找到了一帧完整的NALU单元
                    if(!nalBuffer.empty()){
                        C_H264Enc::NALUnitType NalType = C_H264Enc::GetNALType((unsigned char*)nalBuffer.data(), nalBuffer.size());
                        //NLOG_ERR("NalType=%d\n", NalType);
                        //发送一个完整的NALU单元
                        if(NalType != C_H264Enc::NAL_UNKNOWN){
                            if(m_bNeedIframe && NalType != C_H264Enc::NAL_IDR_PICTURE){
                                //需要关键帧的时候不是spp的NAL跳过，避免终端预览时花屏
                                NLOG_WRN("m_bNeedIframe, not IDR_PICTURE, skip this NAL\n");
                            }else{
                                //是一个正常的NALU单元数据时发送该NALU给对应的客户端
                                SendNal(nalBuffer.data(), nalBuffer.size());
                                if(NalType == C_H264Enc::NAL_IDR_PICTURE || NalType == C_H264Enc::NAL_SLICE){
                                    //如果NALU是一帧正常帧数据时延时30ms，保证文件发送速率接近30fps,视频快进时m_IntervalMs会变小
                                    std::this_thread::sleep_for(std::chrono::milliseconds(m_IntervalMs));
                                }
                                if(m_bNeedIframe && NalType == C_H264Enc::NAL_IDR_PICTURE)
                                    m_bNeedIframe = false;  //发送sps后续不需要I帧
                            }

                        }else{
                            NLOG_ERR("NalType == NAL_UNKNOWN\n");
                        }
                        //发送完毕时清除该NALU单元
                        nalBuffer.clear();
                        readPos += startPos;
                    }else{
                        //nalBuffer为空且在buffer中找到了起始码，先将起始码拷贝到nalBuffer中，否则会一直循环停留在原地
                        nalBuffer.insert(nalBuffer.end(), buffer.begin()+readPos , buffer.begin()+readPos+4);
                        readPos += 4;
                    }

                }else{

                    //没找到下一个NAL单元的起始码时，将缓冲区中剩余的数据全部拷贝到nalBuffer中，等待下一次文件读取使nalBuffer完整
                    nalBuffer.insert(nalBuffer.end(), buffer.begin()+readPos , buffer.end());
                    readPos = bytesRead;
                }
            }
        }
    }
}

//获取文件大小
unsigned int C_ClientConnect::GetFileSize(std::string filePath)
{
    std::ifstream file(filePath, std::ios::binary);
    // 获取文件大小
    file.seekg(0, std::ios::end);
    std::streampos fileSize = file.tellg();
    file.seekg(0, std::ios::beg);
    file.close();

    return fileSize;
}

//获取当前正在发送文件大小
long long C_ClientConnect::GetSendFileSize(){
    std::lock_guard<std::mutex> lock(m_pListener->GetfileMapMutex());
    auto it = m_fileMapIter; 
    ++it;   //判断当前迭代器是不是最后一个map元素
    long long fileSize = it == m_pListener->GetfileMap().end() ? m_pListener->GetLastFileSize() : m_fileMapIter->second;
    return fileSize;
}