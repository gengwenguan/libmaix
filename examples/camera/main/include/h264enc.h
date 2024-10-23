#pragma once
#include "vencoder.h"
#include<memory>
#include<iostream>

class C_h264enc
{
public:
    class C_Listener{
    public:
        virtual int OnOutputH264(unsigned char* data, unsigned int dataLen) = 0;
    };
public:
    C_h264enc(C_Listener* pListener, unsigned int srcWight, unsigned int srcHight, unsigned int dstWidth, unsigned int dstHeight);
    ~C_h264enc();
    //输入NV21采集数据
    int InputData(unsigned char* inputData);
    //强制编码一帧关键帧
    void ForceIframe(){
        std::cout << "ForceIframe()" << std::endl;
        m_forceIframe = true;
    }

private:
    C_Listener* m_pListener;
    VideoEncoder *m_pVideoEnc;  //视频编码器
    VencH264Param m_h264Param;
    VencBaseConfig m_baseConfig;
    VencAllocateBufferParam m_bufferParam;
    VencInputBuffer m_inputBuffer;
    VencOutputBuffer m_outputBuffer;            
    VencHeaderData m_sps_pps_data;
    bool           m_forceIframe = true;

    FILE *out_file = NULL;

};