#include<vector>
#include <arpa/inet.h>
#include <errno.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include"h264Enc.h"
#include"clientConnect.h"
#include"logAdapt.h"

C_ClientConnect::C_ClientConnect(int socketFd, std::map<std::string, unsigned int>& fileMap, std::mutex& fileMapMutex)
    :m_sockeFd(socketFd),
    m_fileMapMutex(fileMapMutex),
    m_fileMap(fileMap),
    m_fileMapIter(fileMap.begin()),
    m_sendFile(m_fileMapIter->first.c_str(), std::ios::binary),
    m_sendFileSize(m_fileMapIter->second),
    m_bRunFlag(true),
    m_pThread( new std::thread( [this]() { this->SendFileData(); }) )
{
    CLOG_INF("m_fileMapIter->first.c_str()=%s m_sendFileSize=%d m_sendFile.is_open()=%d\n", m_fileMapIter->first.c_str(), m_sendFileSize, m_sendFile.is_open());
    //当文件大小为0时尝试重新获取文件大小
    if(m_sendFileSize == 0)
        m_sendFileSize = GetFileSize(m_fileMapIter->first);
}

C_ClientConnect::~C_ClientConnect()
{
    m_bRunFlag = false;
    if(m_pThread != nullptr){
        m_pThread->join();
    }
    CLOG_ERR("this=%p m_sockeFd=%d\n", this, m_sockeFd);
}

//向客户端通过网络发送NAL数据
int C_ClientConnect::SendNal(char* pData, unsigned int nLen)
{
    // 转换整数的字节序为网络字节序
    int networkNumber = htonl(nLen);
    //先将一帧H264数据的长度发送给客户端，长度为4个字节
    int ret = send(m_sockeFd, &networkNumber, sizeof(networkNumber), MSG_NOSIGNAL);
    if(ret>0){
        //再将实际的H264数据发送给客户端
        ret = send(m_sockeFd, pData, nLen, MSG_NOSIGNAL);
    }

    if (ret <= 0) {
        // 发送失败
        CLOG_ERR("send failed, socket:%d\n", m_sockeFd);
    }
    return ret;
}

void C_ClientConnect::SendFileData()
{
    //return;
    // 查找 NAL 起始码的 lambda 表达式
    auto findNALStart = [](char* buffer, size_t bufLen, size_t& startPos) -> bool {
        for (size_t i = 0; i < bufLen - 3; ++i) {
            if (buffer[i] == 0x00 && buffer[i + 1] == 0x00 && buffer[i + 2] == 0x00 && buffer[i + 3] == 0x01) {
                startPos = i;
                return true;
            }
        }
        return false;
    };

    std::vector<char> nalBuffer;             //完整NAL单元缓冲区
    while(m_bRunFlag){

        if(!m_sendFile){
            m_sendFile.close();  //某个文件读取到末尾了关闭该文件，打开下一个文件，让客户端依次播放录制的视频文件
            {
                std::lock_guard<std::mutex> lock(m_fileMapMutex);
                if(++m_fileMapIter == m_fileMap.end()){
                    CLOG_ERR("Failed m_fileMapIter == m_fileMap.end() , exit!\n");
                    return;
                }
            }
            m_sendFile.open(m_fileMapIter->first.c_str(), std::ios::binary);
        }

        //文件中读取部分数据
        std::vector<char> buffer(kTmpBuffSize);        
        m_sendFile.read(buffer.data(), kTmpBuffSize);
        std::streamsize bytesRead = m_sendFile.gcount();
        //CLOG_ERR("bytesRead%d\n", bytesRead);

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
                        C_h264Enc::NALUnitType NalType = C_h264Enc::GetNALType((unsigned char*)nalBuffer.data(), nalBuffer.size());
                        //发送一个完整的NALU单元
                        if(NalType != C_h264Enc::NAL_UNKNOWN){
                            //是一个正常的NALU单元数据时发送该NALU给对应的客户端
                            SendNal(nalBuffer.data(), nalBuffer.size());
                            if(NalType == C_h264Enc::NAL_IDR_PICTURE || NalType == C_h264Enc::NAL_SLICE){
                                std::this_thread::sleep_for(std::chrono::milliseconds(30));
                                //如果NALU是一帧正常帧数据时延时30ms，保证文件发送速率接近30fps
                            }
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




// int C_ClientConnect::get_h264_frame(unsigned char  *buffer, unsigned int length, H264_FRAME_INFO *frame_info)
// {
//     int pos = 0, ret;
//     int bFindFrame = 0;
//     int bFrameType = 0;
//     unsigned int nal_type;
//     frame_info->nalu_num = 0;

//     if (nullptr == buffer || length <=4 )
//     {
//         CM_Log(LOG_LEVEL_INFO, "get_h264_frame length:%d",length);
//         return -1;
//     }

//     //pos += ret;
//     while(1)
//     {
//         int remainLength = length - pos;
//         ret = get_h264_nalu(&buffer[pos], remainLength);
//         if (ret < 0 )
//         {
//             return -1;
//         }

//         if((buffer[pos + 4] & 0x1F) == 5)
//         {
//             bFindFrame = 1;
//             bFrameType = FRAME_TYPE_VIDEO_IFRAME;
//         }

//         if((buffer[pos + 4] & 0x1F) == 9
//            || (buffer[pos + 4] & 0x1F) == 7 )
//         {
//             if( bFindFrame )
//             {
// //                fseek (fp_video_in, (-1 * ret), SEEK_CUR);
//                 break;
//             }
//         }

//         if((buffer[pos + 4] & 0x1F) == 1)
//         {
//             if(bFindFrame == 0)
//             {
//                 bFrameType = FRAME_TYPE_VIDEO_PFRAME;
//                 bFindFrame = 1;
//             }
//             else
//             {
// //                fseek (fp_video_in, (-1 * ret), SEEK_CUR);
//                 break;
//             }
//         }

//         frame_info->nalu[frame_info->nalu_num].nalu_ptr = &buffer[pos];
//         frame_info->nalu[frame_info->nalu_num].nalu_len = ret;
//         frame_info->nalu_num++;
//         pos += ret;

//         if (pos >= length-1)
//         {
//             break;
//         }
//     }

//     if (bFrameType == FRAME_TYPE_VIDEO_IFRAME)
//     {
//         frame_info->frame_type = FRAME_TYPE_VIDEO_IFRAME;

//         CM_Log(LOG_LEVEL_INFO, "get_h264_frame I Frame");
//     }
//     else
//     {
//         frame_info->frame_type = FRAME_TYPE_VIDEO_PFRAME;

//         CM_Log(LOG_LEVEL_INFO, "get_h264_frame P Frame");
//     }

//     return pos;
// }