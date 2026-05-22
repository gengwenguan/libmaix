#include "opusEnc.h"
#include "logAdapt.h"
#include "rtpBase.h"

#define PRINT_ERROR(errnum) do { \
    char errbuf[AV_ERROR_MAX_STRING_SIZE]; \
    av_strerror(errnum, errbuf, sizeof(errbuf)); \
    std::cerr << "Error: " << errbuf << std::endl; \
    CLOG_ERR("Error:%s,%d\n", errbuf, errnum); \
} while (0)

void check_alsa_error(int err, const char *msg) {
    if (err < 0) {
        fprintf(stderr, "ALSA error: %s: %s\n", msg, snd_strerror(err));
        exit(EXIT_FAILURE);
    }
}

constexpr unsigned int SAMPLE_RATE = 48000;
constexpr unsigned long PERIOD_SIZE = 960;  // 20ms at 48kHz
constexpr unsigned int CHANNELS = 1;

C_OpusEnc::C_OpusEnc(C_Listener* pListener)
    :m_pListener(pListener)
{
    int err;
    snd_pcm_hw_params_t *hw_params;

    // 打开音频设备
    err = snd_pcm_open(&m_capture_handle, "default", SND_PCM_STREAM_CAPTURE, 0);
    check_alsa_error(err, "Opening PCM device for capture");

    // 分配和初始化硬件参数
    err = snd_pcm_hw_params_malloc(&hw_params);
    check_alsa_error(err, "Allocating hardware parameters");

    err = snd_pcm_hw_params_any(m_capture_handle, hw_params);
    check_alsa_error(err, "Initializing hardware parameters");

    err = snd_pcm_hw_params_set_access(m_capture_handle, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED);
    check_alsa_error(err, "Setting access type");

    err = snd_pcm_hw_params_set_format(m_capture_handle, hw_params, SND_PCM_FORMAT_S16_LE);
    check_alsa_error(err, "Setting sample format");

    unsigned int smaple_rate = SAMPLE_RATE;
    err = snd_pcm_hw_params_set_rate_near(m_capture_handle, hw_params, &smaple_rate, 0);
    check_alsa_error(err, "Setting sample rate");

    err = snd_pcm_hw_params_set_channels(m_capture_handle, hw_params, CHANNELS);
    check_alsa_error(err, "Setting channel count");

    unsigned long period_size = PERIOD_SIZE;  // 20ms at 48kHz
    err = snd_pcm_hw_params_set_period_size_near(m_capture_handle, hw_params, &period_size, 0);
    check_alsa_error(err, "Setting period size");

    err = snd_pcm_hw_params(m_capture_handle, hw_params);
    check_alsa_error(err, "Setting hardware parameters");

    // 释放硬件参数结构
    snd_pcm_hw_params_free(hw_params);

    // 准备设备
    err = snd_pcm_prepare(m_capture_handle);
    check_alsa_error(err, "Preparing the PCM device");

    // 找到Opus编码器
    AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_OPUS);
    if (!codec) {
        CLOG_ERR("Could not find AV_CODEC_ID_OPUS\n");
        return;
    }

    // 分配编码上下文
    m_codec_ctx = avcodec_alloc_context3(codec);
    if(!m_codec_ctx){
        CLOG_ERR("Could not alloc m_codec_ctx\n");
        return;
    }

    // 设置编码参数
    m_codec_ctx->sample_rate = SAMPLE_RATE;
    m_codec_ctx->channel_layout = AV_CH_LAYOUT_MONO;
    m_codec_ctx->channels = CHANNELS;
    m_codec_ctx->sample_fmt = AV_SAMPLE_FMT_S16;
    m_codec_ctx->bit_rate = 64000; // 根据需要设置比特率

    // 允许使用实验性的编码器
    //m_codec_ctx->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;

    // 打开编码器
    if (avcodec_open2(m_codec_ctx, codec, nullptr) < 0) {
        CLOG_ERR("Could not open m_codec\n");
        return;
    }

    m_bRun = true;
    m_pCaptureEncoderThread = std::unique_ptr<std::thread>(new std::thread( [this]() { this->CaptureEncoder(); }));
}


C_OpusEnc::~C_OpusEnc()
{
    m_bRun = false;
    m_pCaptureEncoderThread->join();

    avcodec_close(m_codec_ctx);
    avcodec_free_context(&m_codec_ctx);

    snd_pcm_close(m_capture_handle);
}

// 音频采集逻辑
void C_OpusEnc::CaptureEncoder()
{
    // 分配AVFrame
    AVFrame* frame = av_frame_alloc();
    if (!frame) {
        CLOG_ERR("Could not alloc frame\n");
        return;
    }
    frame->nb_samples = 960; // 20ms * 48000Hz = 960 samples
    frame->format = AV_SAMPLE_FMT_S16;
    frame->channel_layout = AV_CH_LAYOUT_MONO;
    frame->channels = 1;
    frame->sample_rate = 48000;
    // 分配缓冲区
    if (av_frame_get_buffer(frame, 0) < 0) {
        CLOG_ERR("frame Could not get buffer\n");
        return;
    }
    //申请输出包
    AVPacket *pkt = av_packet_alloc();

    // int starttime = Base_GetTimeTickMs();
    // int count = 0;

    std::unique_ptr<char[]> captureBuffer = std::unique_ptr<char[]>(new char[PERIOD_SIZE * 2]);
    int err;
    while (m_bRun) {
        //CLOG_INF("time:%d  count:%d\n", Base_GetTimeTickMs() - starttime, count++);
        // 读取音频数据
        err = snd_pcm_readi(m_capture_handle, captureBuffer.get(), PERIOD_SIZE);
        if (err == -EPIPE) {
            fprintf(stderr, "Underrun occurred\n");
            snd_pcm_prepare(m_capture_handle);
            continue;
        }
        check_alsa_error(err, "Reading from PCM device");

            //此处可控制pcm文件写入文件，用于临时测试数据是否正常
            if(false){ 
                //智能指针删除器
                auto fileDeleter = [](std::ofstream* pobj){ pobj->close(); delete pobj; };
                //使用静态智能指针，程序退出后资源释放文件正常关闭
                static auto outputFile = std::unique_ptr<std::ofstream, decltype(fileDeleter)>(
                    new std::ofstream("capture.pcm", std::ios::out | std::ios::binary),
                    fileDeleter
                );
                static unsigned int fileSize = 0; //统计写入的文件大小
                //文件正常打开时进行写入
                if(outputFile->is_open()){
                    outputFile->write((const char*)captureBuffer.get(), PERIOD_SIZE * 2);

                    fileSize += (PERIOD_SIZE * 2);
                    if(fileSize > 10 * 1024 * 1024){ //文件大于10M时重新保存
                        fileSize = 0;
                        outputFile->close();
                        outputFile->open("capture.pcm", std::ios::out | std::ios::binary);
                    }
                }
            }


        //CLOG_INF("Sample  %d: %d\n", captureBuffer[0], captureBuffer[PERIOD_SIZE-1]);
        // 编码帧
        frame->data[0] = (unsigned char*)captureBuffer.get();
        frame->nb_samples = 960; // 20ms * 48000Hz = 960 samples
        //frame->pkt_size
        int ret = avcodec_send_frame(m_codec_ctx, frame);
        if (ret < 0) {
            CLOG_ERR("Could not send frame to m_codec_ctx\n");
            PRINT_ERROR(ret);
        }else{
            // 获取编码后的数据
            ret = avcodec_receive_packet(m_codec_ctx, pkt);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                CLOG_ERR("m_codec_ctx no data output\n");
            } else if (ret < 0) {
                PRINT_ERROR(ret);
            } else {
                // 编码后的数据在pkt.data，长度是pkt.size,回调opus编码数据
                m_pListener->OnOutputOpus(pkt->data, pkt->size);
                // 释放pkt
                av_packet_unref(pkt);
            }
        }
    }

    // 释放 AVPacket
    av_packet_free(&pkt);
    // 释放资源
    av_frame_free(&frame);
    CLOG_INF("CaptureEncoder exit\n");
}