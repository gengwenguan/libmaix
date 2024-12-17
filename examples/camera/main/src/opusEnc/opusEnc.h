/********************************************************************************* 
  *Copyright(C),Your Company 
  *FileName:  opusEnc.h
  *Author:    gengwenguan
  *Date:      2024-12-16
  *Description:  音频采集编码为opus编码格式，单通道、16位采样深度，48k采样率，64k码率
**********************************************************************************/ 
#pragma once
#include<memory>
#include<iostream>
#include <thread>
#include <mutex>
extern "C" {
    #include <libavutil/avutil.h>
    #include <libavdevice/avdevice.h>
    #include <libavformat/avformat.h>
    #include <libavutil/fifo.h>
}

class C_OpusEnc
{
private:
    static constexpr int kFifoSize = 1024 * 96;      //设置96k的缓存大小
    static constexpr int kEncBuffSize = 48 * 20 * 2; //音频编码缓冲区大小，48k 20ms 采样深度16位(2字节)
public:
    class C_Listener{
    public:
        virtual int OnOutputOpus(unsigned char* data, unsigned int dataLen) = 0;
    };
public:
    C_OpusEnc(C_Listener* pListener);
    ~C_OpusEnc();

private:
    //音频采集线程对应的逻辑函数
    void CaptureAudio();
    //音频编码线程对应的逻辑函数
    void EncoderAudio();
private:
    C_Listener* m_pListener;
    AVFormatContext *m_input_ctx = nullptr;
    AVDictionary *m_options = nullptr;

    int m_audio_stream_index;

    std::mutex m_pFifoMutex;
    AVFifoBuffer *m_pFifo;

    bool m_bRun;
    std::unique_ptr<std::thread> m_pCaptureThread;    //音频采集线程
    std::unique_ptr<std::thread> m_pEncoderThread;    //音频采集线程

};