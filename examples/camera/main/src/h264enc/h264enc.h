/********************************************************************************* 
  *Copyright(C),Your Company 
  *FileName:  h264enc.h
  *Author:    gengwenguan
  *Date:      2024-10-25
  *Description:  h264编码类，可将输入的NV21格式YUV数据编码为H264数据
                 提供强制编码关键帧接口
**********************************************************************************/ 
#pragma once
#include<memory>
#include<iostream>
#include "vencoder.h"

class C_h264enc
{
private:
    // 定义NAL单元类型的枚举
    enum NALUnitType {
        NAL_UNKNOWN = 0,
        NAL_SLICE = 1,       //P帧或B帧
        NAL_IDR_PICTURE = 5, //I帧
        NAL_SEI = 6,
        NAL_SPS = 7,
        NAL_PPS = 8
        // 其他NAL单元类型可以根据需要继续添加
    };
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
    //强制编码一帧关键帧，此处进行标记，实际在送数据时进行控制强制I帧
    void ForceIframe(){
        std::cout << "ForceIframe()" << std::endl;
        m_forceIframe = true;
    }
    
private:
    // 判断NAL单元类型
    NALUnitType getNALType(unsigned char* data, unsigned int dataLen);

private:
    C_Listener* m_pListener;
    VideoEncoder *m_pVideoEnc;       //视频编码器
    VencH264Param m_h264Param;
    VencBaseConfig m_baseConfig;
    VencAllocateBufferParam m_bufferParam;
    VencInputBuffer m_inputBuffer;
    VencOutputBuffer m_outputBuffer;
    VencHeaderData m_sps_pps_data;
    bool           m_forceIframe = true;

    FILE *out_file = NULL;

};