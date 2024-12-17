#include<string.h>
#include<stdio.h>
#include<string>
#include<iostream>
#include "memoryAdapter.h"
#include "h264Enc.h"
#include "logAdapt.h"

//输入的YUV分辨率和编码输出的h264分辨率
C_H264Enc::C_H264Enc(C_Listener* pListener, unsigned int srcWight, unsigned int srcHight, unsigned int dstWidth, unsigned int dstHeight)
    :m_pListener(pListener)
{
    m_h264Param.bEntropyCodingCABAC = 1;
    m_h264Param.nBitrate = dstWidth*dstHeight;  //码率设置为分辨率
    m_h264Param.nFramerate = 30;
    m_h264Param.nCodingMode = VENC_FRAME_CODING;
    m_h264Param.nMaxKeyInterval = 60 * 1000; //60秒一个关键帧，此处配置似乎并不生效，实际编码好像每1~2秒会出一个I帧
    m_h264Param.sProfileLevel.nProfile = VENC_H264ProfileMain;
    m_h264Param.sProfileLevel.nLevel = VENC_H264Level31;
    m_h264Param.sQPRange.nMinqp = 5;  //qp值越小画面越清晰
    m_h264Param.sQPRange.nMaxqp = 40;
    m_h264Param.sRcParam.eRcMode = AW_VBR;  //采用动码率VBR，有效降低静态画面编码码率
    m_h264Param.sRcParam.sVbrParam.uMaxBitRate = dstWidth*dstHeight*3; //vbr最大编码三倍正常码率
    m_h264Param.sRcParam.sVbrParam.nQuality = 9;
    m_h264Param.sRcParam.sVbrParam.nMovingTh = 20;

    memset(&m_baseConfig, 0, sizeof(VencBaseConfig));
    memset(&m_bufferParam, 0, sizeof(VencAllocateBufferParam));
    m_baseConfig.memops = MemAdapterGetOpsS();
    if (m_baseConfig.memops == NULL)
        CLOG_INF("MemAdapterGetOpsS failed\n");
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
    CLOG_INF("VideoEncInit: %d\n", ret);
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

C_H264Enc::~C_H264Enc()
{
    CdcMemClose(m_baseConfig.memops);
    if(m_pVideoEnc != nullptr){
        ReleaseAllocInputBuffer(m_pVideoEnc);
        VideoEncUnInit(m_pVideoEnc);
        VideoEncDestroy(m_pVideoEnc);
    }
}

//输入NV21采集数据
int C_H264Enc::InputData(unsigned char* inputData)
{
    //真正的强制I帧在送数据时进行控制
    if(m_forceIframe){
        CLOG_INF("forceIframe\n");
        int value = 1;
        // 强制编码器编I帧
        VideoEncSetParameter(m_pVideoEnc, VENC_IndexParamForceKeyFrame, &value);
        m_forceIframe = false;
        //sps pps信息
        VideoEncGetParameter(m_pVideoEnc, VENC_IndexParamH264SPSPPS, &m_sps_pps_data);
        m_pListener->OnOutputH264(m_sps_pps_data.pBuffer, m_sps_pps_data.nLength);
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
        //回调编码出的h264数据,
        m_pListener->OnOutputH264(m_outputBuffer.pData0, m_outputBuffer.nSize0);
        if (m_outputBuffer.nSize1)
        {
            m_pListener->OnOutputH264(m_outputBuffer.pData1, m_outputBuffer.nSize1);
        }

        FreeOneBitStreamFrame(m_pVideoEnc, &m_outputBuffer);
    }
    return 0;
}

// 判断NAL单元类型
C_H264Enc::NALUnitType C_H264Enc::GetNALType(unsigned char* data, unsigned int dataLen)
{
    size_t pos = 0;

    // 跳过起始码
    if (dataLen >= 4 && data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x00 && data[3] == 0x01) {
        pos = 4;
    } else if (dataLen >= 3 && data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x01) {
        pos = 3;
    } else {
        CLOG_INF("Invalid NAL unit format.\n");
        return NAL_UNKNOWN; // 无效的NAL单元类型
    }

    return static_cast<NALUnitType>(data[pos] & 0x1F);  // 获取NAL单元类型的低5位
}

/******************************************************************************
* 功  能：获取一个h.264 nalu
* 参  数：buffer - 目标缓冲区
* 返回值：返回错误码或nalu长度
******************************************************************************/
int C_H264Enc::get_h264_nalu(unsigned char  *buffer, unsigned length)
{
    unsigned pos = 0;

    //先取出第一个起始码 0x00000001
//    while(!feof(fp_video_in) && (buffer[pos++] = fgetc(fp_video_in)) == 0);
//    if(feof(fp_video_in) || buffer[pos - 1] != 1 || pos < 4)
//    {
//        return -1;
//    }

    while (pos < length-1 && buffer[pos++] == 0);
    if (buffer[pos - 1] != 1 || pos < 4)
    {
        return -1;
    }

    //连续读取码流直到出现下一个起始码 0x00000001
    do
    {
//		buffer[pos++] = fgetc(fp_video_in);
        pos++;
        if (pos >= length)
        {
            return (pos);
        }
    } while( buffer[pos-4] != 0 ||buffer[pos-3] != 0 || buffer[pos-2] != 0 || buffer[pos-1] != 1);


    return (pos - 4);
}
