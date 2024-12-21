/********************************************************************************* 
  *Copyright(C),Your Company 
  *FileName:  terminal.h
  *Author:    gengwenguan
  *Date:      2024-10-25
  *Description:  编码传输终端，支持输入RGB888或NV21格式视频，该类会将
                 YUV数据编码为h264发送给连接的客户端
**********************************************************************************/ 
#pragma once
#include <fstream>
#include "libmaix_cam.h"
#include "libmaix_disp.h"
#include "h264Enc.h"
#include "tcpServer.h"
#include "fileMng.h"
#include "opusEnc.h"

class C_Terminal : public C_H264Enc::C_Listener,
                   public C_TcpServer::C_Listener,
                   public C_FileMng::C_Listener,
                   public C_OpusEnc::C_Listener
{
public:
    C_Terminal(unsigned int Wight, unsigned int Hight);
    ~C_Terminal();

    //送人采集数据
    int InputRgb888(unsigned char* inputData);

    //送人采集数据
    int InputNv21(unsigned char* inputData);

    //获取设备的ipv4地址
    static std::string get_ipv4_address();

private:
    //rgb888转Nv21格式
    void rgb888ToNv21(const unsigned char* rgb, unsigned char* nv21, int width, int height);
private:
    //编码器回调的H264数据
    int OnOutputH264(unsigned char* data, unsigned int dataLen) override;

    //音频编码回调的opus数据
    int OnOutputOpus(unsigned char* data, unsigned int dataLen) override;

    /*新客户端连接事件*/
    int OnNewClientConnect(int fd) override;

    /*新文件创建*/
    int OnNewFileCreate() override;
private:
    unsigned int m_Wight;
    unsigned int m_Hight;

    std::unique_ptr<C_H264Enc>   m_pH264Enc;
    std::unique_ptr<C_OpusEnc>   m_pOpusEnc;
    std::unique_ptr<C_TcpServer> m_pTcpServer;
    std::unique_ptr<C_FileMng>   m_pFileMng;

    std::unique_ptr<unsigned char[]> m_pNv12Buff;

};