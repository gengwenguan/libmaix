#include "h264enc.h"
#include <string.h>
#include "memoryAdapter.h"
#include <stdio.h>
#include<string>
#include<iostream>

C_h264enc::C_h264enc(C_Listener* pListener, unsigned int srcWight, unsigned int srcHight, unsigned int dstWidth, unsigned int dstHeight)
    :m_pListener(pListener)
{
    m_h264Param.bEntropyCodingCABAC = 1;
    m_h264Param.nBitrate = 256*1024;
    m_h264Param.nFramerate = 30;
    m_h264Param.nCodingMode = VENC_FRAME_CODING;
    m_h264Param.nMaxKeyInterval = 30 * 60; //帧率乘60，相当于最大60秒一个关键帧
    m_h264Param.sProfileLevel.nProfile = VENC_H264ProfileMain;
    m_h264Param.sProfileLevel.nLevel = VENC_H264Level31;
    m_h264Param.sQPRange.nMinqp = 5;  //qp值越小画面越清晰
    m_h264Param.sQPRange.nMaxqp = 40;
    m_h264Param.sRcParam.eRcMode = AW_VBR;  //采用动码率VBR，有效降低静态画面编码码率
    m_h264Param.sRcParam.sVbrParam.uMaxBitRate = 256*1024*3; //vbr最大编码三倍正常码率
    m_h264Param.sRcParam.sVbrParam.nQuality = 9;
    m_h264Param.sRcParam.sVbrParam.nMovingTh = 20;

    memset(&m_baseConfig, 0, sizeof(VencBaseConfig));
    memset(&m_bufferParam, 0, sizeof(VencAllocateBufferParam));
    m_baseConfig.memops = MemAdapterGetOpsS();
    if (m_baseConfig.memops == NULL)
        printf("MemAdapterGetOpsS failed\n");
    CdcMemOpen(m_baseConfig.memops);
    m_baseConfig.nInputWidth = srcWight;
    m_baseConfig.nInputHeight = srcHight;
    m_baseConfig.nStride = srcWight;
    m_baseConfig.nDstWidth = dstWidth;
    m_baseConfig.nDstHeight = dstHeight;
    // the format of yuv file is yuv420p,
    // but the old ic only support the yuv420sp,
    // so use the func yu12_nv12() to config all the format.
    m_baseConfig.eInputFormat =  VENC_PIXEL_YVU420SP;

    // Y  C 的地址
    m_bufferParam.nSizeY = m_baseConfig.nInputWidth * m_baseConfig.nInputHeight ;
    m_bufferParam.nSizeC = m_baseConfig.nInputWidth * m_baseConfig.nInputHeight /2 ;
    m_bufferParam.nBufferNum = 2;
    //创建编码器
    m_pVideoEnc = VideoEncCreate(VENC_CODEC_H264_VER2);
    int value;
    VideoEncSetParameter(m_pVideoEnc, VENC_IndexParamH264Param, &m_h264Param);
    value = 0;
    VideoEncSetParameter(m_pVideoEnc, VENC_IndexParamIfilter, &value);
    value = 0; // degree
    VideoEncSetParameter(m_pVideoEnc, VENC_IndexParamRotation, &value);
    value = 0;
    VideoEncSetParameter(m_pVideoEnc, VENC_IndexParamSetPSkip, &value);
    int ret = -1;
    ret = VideoEncInit(m_pVideoEnc, &m_baseConfig);
    std::cout << "VideoEncInit:" << ret << std::endl;
    //VideoEncGetParameter(pVideoEnc, VENC_IndexParamH264SPSPPS, &sps_pps_data);
    //fwrite(sps_pps_data.pBuffer, 1, sps_pps_data.nLength, out_file);
    //printf("*****************************\n");
    //logd("sps_pps_data.nLength: %d", sps_pps_data.nLength);
    //for (int head_num = 0; head_num < sps_pps_data.nLength; head_num++)
    //    logd("the sps_pps :%02x\n", *(sps_pps_data.pBuffer + head_num));
    //printf("*****************************\n");
    AllocInputBuffer(m_pVideoEnc, &m_bufferParam);
    m_inputBuffer.bEnableCorp = 0;
    m_inputBuffer.sCropInfo.nLeft = 0;
    m_inputBuffer.sCropInfo.nTop = 0;
    m_inputBuffer.sCropInfo.nWidth = srcWight;
    m_inputBuffer.sCropInfo.nHeight = srcHight;
}

C_h264enc::~C_h264enc()
{
    std::cout << "~C_h264enc()" << std::endl;
    CdcMemClose(m_baseConfig.memops);
    if(m_pVideoEnc != nullptr){
        ReleaseAllocInputBuffer(m_pVideoEnc);
        VideoEncUnInit(m_pVideoEnc);
        VideoEncDestroy(m_pVideoEnc);
    }
    std::cout << "~C_h264enc() end!" << std::endl;
}

int C_h264enc::InputData(unsigned char* inputData)
{
    if(m_forceIframe){
        std::cout << "forceIframe" << std::endl;
        int value = 1;
        // 文件第一帧数据保存时强制编码器编I帧，保证视频打开时第一帧就能正常播放
        VideoEncSetParameter(m_pVideoEnc, VENC_IndexParamForceKeyFrame, &value);
        //创建文件后将sps pps信息写入文件
        VideoEncGetParameter(m_pVideoEnc, VENC_IndexParamH264SPSPPS, &m_sps_pps_data);
        //fwrite(m_sps_pps_data.pBuffer, 1, m_sps_pps_data.nLength, out_file);   
        m_pListener->OnOutputH264(m_sps_pps_data.pBuffer, m_sps_pps_data.nLength);
        m_forceIframe = false;
    }

    GetOneAllocInputBuffer(m_pVideoEnc, &m_inputBuffer);
    memcpy(m_inputBuffer.pAddrVirY, inputData, m_bufferParam.nSizeY);
    memcpy(m_inputBuffer.pAddrVirC, (unsigned char *)(inputData + m_bufferParam.nSizeY), m_bufferParam.nSizeC);
    FlushCacheAllocInputBuffer(m_pVideoEnc, &m_inputBuffer);
    AddOneInputBuffer(m_pVideoEnc, &m_inputBuffer);
    VideoEncodeOneFrame(m_pVideoEnc);

    AlreadyUsedInputBuffer(m_pVideoEnc, &m_inputBuffer);
    ReturnOneAllocInputBuffer(m_pVideoEnc, &m_inputBuffer);    

    int ret = GetOneBitstreamFrame(m_pVideoEnc, &m_outputBuffer);

    if(-1 != ret)
    {
        // if(out_file == NULL)        
        // {

        //     std::string filePath = "tmp.264"; //根据当前时间和保存位置生成文件绝对路径

        //     printf("new video file path: %s\n", filePath.c_str());

        //     out_file = fopen(filePath.c_str(),"wb");            
        //     if (!out_file) {
        //         std::cerr << "Failed to open file: " << filePath << std::endl;
        //         return -1; // or handle the error appropriately
        //     }
        //     //创建文件后将sps pps信息写入文件
        //     VideoEncGetParameter(m_pVideoEnc, VENC_IndexParamH264SPSPPS, &m_sps_pps_data);
        //     fwrite(m_sps_pps_data.pBuffer, 1, m_sps_pps_data.nLength, out_file);            
        // }


        //取出数据，写入文件
        //fwrite(m_outputBuffer.pData0, 1, m_outputBuffer.nSize0, out_file);
        m_pListener->OnOutputH264(m_outputBuffer.pData0, m_outputBuffer.nSize0);
        //mp4Encoder.WriteH264Data(handle_Mp4File,outputBuffer.pData0,outputBuffer.nSize0);
        if (m_outputBuffer.nSize1)
        {
            //fwrite(m_outputBuffer.pData1, 1, m_outputBuffer.nSize1, out_file);
            m_pListener->OnOutputH264(m_outputBuffer.pData1, m_outputBuffer.nSize1);
            //mp4Encoder.WriteH264Data(handle_Mp4File,outputBuffer.pData1,outputBuffer.nSize1);
        }
        

        FreeOneBitStreamFrame(m_pVideoEnc, &m_outputBuffer);

    }
    return 0;
}



