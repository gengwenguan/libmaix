#include "opusEnc.h"
#include "logAdapt.h"

#define PRINT_ERROR(errnum) do { \
    char errbuf[AV_ERROR_MAX_STRING_SIZE]; \
    av_strerror(errnum, errbuf, sizeof(errbuf)); \
    std::cerr << "Error: " << errbuf << std::endl; \
    CLOG_ERR("Error:%s,%d\n", errbuf, errnum); \
} while (0)


C_OpusEnc::C_OpusEnc(C_Listener* pListener)
    :m_pListener(pListener),
    m_audio_stream_index(-1)
{
    // 初始化 FFmpeg
    avdevice_register_all();
    //av_register_all();

    // 打开音频设备

    // 设置设备参数
    av_dict_set(&m_options, "channels", "1", 0);  // 单声道
    av_dict_set(&m_options, "sample_rate", "48000", 0);  // 48000 Hz
    av_dict_set(&m_options, "sample_fmt", "s16", 0);  // 采样格式为16位
    //av_dict_set(&m_options, "fragment_size", "1920", 0);  // 设置缓冲区大小
    //av_dict_set(&m_options, "buffer_size", "1920", 0);  // 设置缓冲区大小

    // 设置设备名称和格式
    const char *device_name = "hw:0,0";  // 默认音频设备
    const char *format_name = "alsa";   // 使用 alsa 作为输入设备

    // 打开输入流
    int ret = avformat_open_input(&m_input_ctx, device_name, av_find_input_format(format_name), &m_options);
    if (ret < 0) {
        PRINT_ERROR(ret);
        return;
    }

    // 查找流信息
    ret = avformat_find_stream_info(m_input_ctx, nullptr);
    if (ret < 0) {
        PRINT_ERROR(ret);
        avformat_close_input(&m_input_ctx);
        return;
    }

    // 查找音频流
    for (unsigned int i = 0; i < m_input_ctx->nb_streams; i++) {
        if (m_input_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            m_audio_stream_index = i;
            break;
        }
    }
    if (m_audio_stream_index == -1) {
        CLOG_ERR("Could not find audio stream.\n");
        avformat_close_input(&m_input_ctx);
        return ;
    }

    //fifo缓存大小
    m_pFifo = av_fifo_alloc(kFifoSize);

    m_bRun = true;
    m_pCaptureThread = std::unique_ptr<std::thread>(new std::thread( [this]() { this->CaptureAudio(); }));
    m_pEncoderThread = std::unique_ptr<std::thread>(new std::thread( [this]() { this->EncoderAudio(); }));
    //AVFifoBuffer *av_fifo_alloc(unsigned int size);
}


C_OpusEnc::~C_OpusEnc()
{
    m_bRun = false;
    m_pCaptureThread->join();
    m_pEncoderThread->join();

    avformat_close_input(&m_input_ctx);
    av_dict_free(&m_options);
}

// 音频采集逻辑
void C_OpusEnc::CaptureAudio()
{
    // 读取音频数据并写入文件
    AVPacket *pkt = av_packet_alloc();
    if (!pkt) {
        CLOG_ERR("Failed to allocate AVPacket\n");
        return;  // 或其他错误处理逻辑
    }

    while (m_bRun && av_read_frame(m_input_ctx, pkt) >= 0) {
        if (pkt->stream_index == m_audio_stream_index) {
            //将从声卡采集的数据写入到fifo缓冲区中
            {
                std::lock_guard<std::mutex> lock(m_pFifoMutex);
                if (av_fifo_space(m_pFifo) < pkt->size) {
                    CLOG_ERR("Not enough space in FIFO to write data\n");
                }else{
                    av_fifo_generic_write(m_pFifo, pkt->data, pkt->size, NULL);
                }
            }
            // CLOG_INF("pkt.size=%d\n", pkt->size);
            // fwrite(pkt->data, 1, pkt->size, pcm_file);
        }
        av_packet_unref(pkt);
    }

    // 释放 AVPacket
    av_packet_free(&pkt);
}

//音频编码线程对应的逻辑函数
void C_OpusEnc::EncoderAudio()
{
    int bytes_read;
    std::unique_ptr<char[]> EncBuff = std::unique_ptr<char[]>(new char[kEncBuffSize]);
    while(m_bRun){
        {
            std::lock_guard<std::mutex> lock(m_pFifoMutex);
            bytes_read = av_fifo_generic_read(m_pFifo, EncBuff.get(), kEncBuffSize, NULL);
            if(bytes_read != kEncBuffSize) continue;
        }

    }

}