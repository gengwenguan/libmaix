/*********************************************************************************
  *Copyright(C),Your Company
  *FileName:  fmp4Muxer.cpp
  *Author:    gengwenguan
  *Date:      2026-05-23
  *Description:  fMP4 muxer 实现，使用 FFmpeg avformat
**********************************************************************************/
#include "fmp4Muxer.h"
#include "logAdapt.h"
#include <cstring>

constexpr AVRational C_Fmp4Muxer::kTbVideo90k;

#define MUX_PRINT_ERROR(errnum) do { \
    char errbuf[AV_ERROR_MAX_STRING_SIZE]; \
    av_strerror(errnum, errbuf, sizeof(errbuf)); \
    CLOG_ERR("fmp4Muxer ffmpeg error: %s (%d)\n", errbuf, errnum); \
} while (0)

// avio 内部缓冲大小：64KB 足够一个 fragment
static constexpr int kAvioBufSize = 64 * 1024;

C_Fmp4Muxer::C_Fmp4Muxer(C_Listener* pListener,
                         const VideoConfig& v,
                         const AudioConfig& a)
    : m_pListener(pListener)
{
    int ret = 0;

    // 1) 创建 mp4 输出 context（fragmented 模式）
    ret = avformat_alloc_output_context2(&m_fmtCtx, nullptr, "mp4", nullptr);
    if (ret < 0 || !m_fmtCtx) {
        CLOG_ERR("avformat_alloc_output_context2 mp4 failed: %d\n", ret);
        return;
    }

    // 2) 自定义 AVIO：把 muxer 的输出截到内存
    m_ioBuf = (uint8_t*)av_malloc(kAvioBufSize);
    if (!m_ioBuf) { CLOG_ERR("av_malloc avio buf fail\n"); return; }
    m_ioCtx = avio_alloc_context(m_ioBuf, kAvioBufSize, 1 /*write*/, this,
                                 nullptr, &OnAvioWrite, nullptr);
    if (!m_ioCtx) { CLOG_ERR("avio_alloc_context fail\n"); return; }
    m_fmtCtx->pb = m_ioCtx;
    m_fmtCtx->flags |= AVFMT_FLAG_CUSTOM_IO;

    // 3) 视频流（H.264），time_base 用 90kHz（mov 标准）
    m_vStream = avformat_new_stream(m_fmtCtx, nullptr);
    if (!m_vStream) { CLOG_ERR("new vstream fail\n"); return; }
    m_vStream->id                         = m_fmtCtx->nb_streams - 1;
    m_vStream->time_base                  = kTbVideo90k;
    m_vStream->codecpar->codec_type       = AVMEDIA_TYPE_VIDEO;
    m_vStream->codecpar->codec_id         = AV_CODEC_ID_H264;
    m_vStream->codecpar->width            = v.width;
    m_vStream->codecpar->height           = v.height;
    m_vStream->codecpar->format           = AV_PIX_FMT_YUV420P;
    m_vStream->codecpar->profile          = FF_PROFILE_H264_MAIN;
    m_vStream->codecpar->level            = 31;

    // 把 SPS/PPS 转成 avcC，作为 extradata
    std::vector<uint8_t> avcC;
    if (!BuildAvcC(v.spsPpsAnnexB, v.spsPpsLen, avcC)) {
        CLOG_ERR("BuildAvcC failed\n");
        return;
    }
    m_vStream->codecpar->extradata = (uint8_t*)av_malloc(avcC.size() + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!m_vStream->codecpar->extradata) { CLOG_ERR("alloc vextra fail\n"); return; }
    memcpy(m_vStream->codecpar->extradata, avcC.data(), avcC.size());
    memset(m_vStream->codecpar->extradata + avcC.size(), 0, AV_INPUT_BUFFER_PADDING_SIZE);
    m_vStream->codecpar->extradata_size = (int)avcC.size();

    // 4) 音频流（AAC-LC），time_base 用采样率（mov 标准）
    m_aStream = avformat_new_stream(m_fmtCtx, nullptr);
    if (!m_aStream) { CLOG_ERR("new astream fail\n"); return; }
    m_aStream->id                         = m_fmtCtx->nb_streams - 1;
    m_aStream->time_base                  = AVRational{1, a.sampleRate};
    m_aStream->codecpar->codec_type       = AVMEDIA_TYPE_AUDIO;
    m_aStream->codecpar->codec_id         = AV_CODEC_ID_AAC;
    m_aStream->codecpar->profile          = FF_PROFILE_AAC_LOW;
    m_aStream->codecpar->sample_rate      = a.sampleRate;
    m_aStream->codecpar->channels         = a.channels;
    m_aStream->codecpar->channel_layout   = (a.channels == 1) ? AV_CH_LAYOUT_MONO : AV_CH_LAYOUT_STEREO;
    m_aStream->codecpar->format           = AV_SAMPLE_FMT_FLTP;
    m_aStream->codecpar->frame_size       = 1024;   // AAC LC 固定 1024 samples/frame
    m_audioSampleRate                     = a.sampleRate;

    if (a.asc && a.ascLen > 0) {
        m_aStream->codecpar->extradata = (uint8_t*)av_malloc(a.ascLen + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!m_aStream->codecpar->extradata) { CLOG_ERR("alloc aextra fail\n"); return; }
        memcpy(m_aStream->codecpar->extradata, a.asc, a.ascLen);
        memset(m_aStream->codecpar->extradata + a.ascLen, 0, AV_INPUT_BUFFER_PADDING_SIZE);
        m_aStream->codecpar->extradata_size = (int)a.ascLen;
    }

    // 5) movflags = fragmented MP4，MSE 友好
    //    frag_keyframe       ：每个 IDR 切 fragment（依赖编码器周期出 IDR）
    //    empty_moov          ：moov 不写 sample table，纯 init 段
    //    default_base_moof   ：每段 moof 的 trun.base_data_offset=0，提高 MSE 兼容性
    //    negative_cts_offsets：避免 cts < dts 时 muxer 抱怨
    //  注意：不要加 "dash"！它会在每个 fragment 前插入 sidx box，
    //        MSE SourceBuffer(segments 模式) 收到 sidx 后会一直等下一个 mdat
    //        导致 "缓冲中..." 卡死（实测 ffprobe 本地能解但浏览器无法播放）
    AVDictionary* opt = nullptr;
    av_dict_set(&opt, "movflags",
                "frag_keyframe+empty_moov+default_base_moof+negative_cts_offsets", 0);
    av_dict_set(&opt, "frag_duration", "500000", 0);   // 500ms 兜底

    // 6) 写头部：触发 ftyp+moov 输出
    ret = avformat_write_header(m_fmtCtx, &opt);
    av_dict_free(&opt);
    if (ret < 0) {
        MUX_PRINT_ERROR(ret);
        return;
    }
    avio_flush(m_ioCtx);

    // 7) 此时 m_accumBuf 里就是 init segment（ftyp+moov）
    if (!m_accumBuf.empty()) {
        if (m_pListener) m_pListener->OnInitSegment(m_accumBuf.data(), m_accumBuf.size());
        CLOG_INF("fmp4Muxer init segment: %zu bytes\n", m_accumBuf.size());
        m_accumBuf.clear();
        m_bInitEmitted = true;
    }

    m_bReady = true;
    CLOG_INF("fmp4Muxer ready (video %dx%d profile=Main level=3.1, audio %dHz/%dch)\n",
             v.width, v.height, a.sampleRate, a.channels);
}

C_Fmp4Muxer::~C_Fmp4Muxer()
{
    std::lock_guard<std::mutex> lk(m_muxMu);
    if (m_fmtCtx) {
        if (m_bReady) {
            // 写 trailer 主要为了让 muxer 释放内部状态干净（live 流不强求 mfra）
            av_write_trailer(m_fmtCtx);
            if (m_ioCtx) avio_flush(m_ioCtx);
        }
        avformat_free_context(m_fmtCtx);
        m_fmtCtx = nullptr;
    }
    if (m_ioCtx) {
        // ioCtx 内部 buffer 字段可能被 ffmpeg 重新分配，要用其当前指针释放
        if (m_ioCtx->buffer) av_freep(&m_ioCtx->buffer);
        avio_context_free(&m_ioCtx);
    }
    CLOG_INF("~C_Fmp4Muxer end\n");
}

int C_Fmp4Muxer::OnAvioWrite(void* opaque, uint8_t* buf, int bufSize)
{
    auto* self = static_cast<C_Fmp4Muxer*>(opaque);
    self->m_accumBuf.insert(self->m_accumBuf.end(), buf, buf + bufSize);
    return bufSize;
}

void C_Fmp4Muxer::FlushBufferAsFragment()
{
    if (m_accumBuf.empty()) return;
    if (m_pListener && m_bInitEmitted) {
        static int s_fragCnt = 0;
        ++s_fragCnt;
        if (s_fragCnt <= 3 || (s_fragCnt % 50) == 0) {
            CLOG_INF("fmp4Muxer OnFragment #%d size=%zu\n", s_fragCnt, m_accumBuf.size());
        }
        m_pListener->OnFragment(m_accumBuf.data(), m_accumBuf.size());
    }
    m_accumBuf.clear();
}

// Annex-B 多 NAL → AVCC（每个 NAL 前 4 字节大端长度）
unsigned int C_Fmp4Muxer::AnnexBToAvcc(const uint8_t* in, unsigned int inLen,
                                       std::vector<uint8_t>& out)
{
    out.clear();
    out.reserve(inLen);
    unsigned int i = 0;
    while (i < inLen) {
        // 查找当前 NAL 的 startcode
        unsigned int sc = 0;
        if (i + 4 <= inLen && in[i]==0 && in[i+1]==0 && in[i+2]==0 && in[i+3]==1) sc = 4;
        else if (i + 3 <= inLen && in[i]==0 && in[i+1]==0 && in[i+2]==1)         sc = 3;
        else { ++i; continue; }

        unsigned int nalStart = i + sc;
        // 查找下一个 startcode
        unsigned int nalEnd = inLen;
        for (unsigned int j = nalStart; j + 3 <= inLen; ++j) {
            if (in[j]==0 && in[j+1]==0) {
                if (j + 4 <= inLen && in[j+2]==0 && in[j+3]==1) { nalEnd = j; break; }
                if (in[j+2]==1)                                  { nalEnd = j; break; }
            }
        }
        unsigned int nalLen = nalEnd - nalStart;
        if (nalLen == 0) { i = nalEnd; continue; }

        // 写 4 字节大端长度
        uint8_t lenBE[4] = {
            (uint8_t)((nalLen >> 24) & 0xFF),
            (uint8_t)((nalLen >> 16) & 0xFF),
            (uint8_t)((nalLen >>  8) & 0xFF),
            (uint8_t)( nalLen        & 0xFF)
        };
        out.insert(out.end(), lenBE, lenBE + 4);
        out.insert(out.end(), in + nalStart, in + nalEnd);
        i = nalEnd;
    }
    return (unsigned int)out.size();
}

// 从 Annex-B 的 SPS+PPS 序列构造 avcC（ISO/IEC 14496-15 5.2.4.1.1）
//   结构（大端）：
//   uint8  configurationVersion = 1
//   uint8  AVCProfileIndication
//   uint8  profile_compatibility
//   uint8  AVCLevelIndication
//   uint8  reserved(6 bits)=111111 + lengthSizeMinusOne(2 bits)=11   => 0xFF
//   uint8  reserved(3 bits)=111 + numOfSPS(5 bits)
//   for each SPS:
//     uint16 spsLen
//     bytes  spsNALU
//   uint8  numOfPPS
//   for each PPS:
//     uint16 ppsLen
//     bytes  ppsNALU
bool C_Fmp4Muxer::BuildAvcC(const unsigned char* spsPps, unsigned int len,
                            std::vector<uint8_t>& avcC)
{
    if (!spsPps || len < 4) return false;

    // 切分 NAL
    std::vector<std::pair<const uint8_t*, unsigned int>> nals; // {payload_start, payload_len}
    unsigned int i = 0;
    while (i < len) {
        unsigned int sc = 0;
        if (i + 4 <= len && spsPps[i]==0 && spsPps[i+1]==0 && spsPps[i+2]==0 && spsPps[i+3]==1) sc = 4;
        else if (i + 3 <= len && spsPps[i]==0 && spsPps[i+1]==0 && spsPps[i+2]==1)              sc = 3;
        else { ++i; continue; }

        unsigned int nalStart = i + sc;
        unsigned int nalEnd = len;
        for (unsigned int j = nalStart; j + 3 <= len; ++j) {
            if (spsPps[j]==0 && spsPps[j+1]==0) {
                if (j + 4 <= len && spsPps[j+2]==0 && spsPps[j+3]==1) { nalEnd = j; break; }
                if (spsPps[j+2]==1)                                   { nalEnd = j; break; }
            }
        }
        if (nalEnd > nalStart) nals.push_back({spsPps + nalStart, nalEnd - nalStart});
        i = nalEnd;
    }

    // 找出 SPS / PPS
    const uint8_t* sps = nullptr; unsigned int spsLen = 0;
    const uint8_t* pps = nullptr; unsigned int ppsLen = 0;
    for (auto& n : nals) {
        unsigned int t = n.first[0] & 0x1F;
        if (t == 7) { sps = n.first; spsLen = n.second; }
        else if (t == 8) { pps = n.first; ppsLen = n.second; }
    }
    if (!sps || !pps || spsLen < 4) {
        CLOG_ERR("BuildAvcC: SPS/PPS not found in extradata\n");
        return false;
    }

    // SPS[1..3] = profile_idc / profile_compat / level_idc
    avcC.clear();
    avcC.push_back(0x01);             // configurationVersion
    avcC.push_back(sps[1]);           // AVCProfileIndication
    avcC.push_back(sps[2]);           // profile_compatibility
    avcC.push_back(sps[3]);           // AVCLevelIndication
    avcC.push_back(0xFF);             // reserved | lengthSizeMinusOne(=3 → NAL 长度 4 字节)
    avcC.push_back(0xE1);             // reserved 111 + numOfSPS=1
    avcC.push_back((spsLen >> 8) & 0xFF);
    avcC.push_back( spsLen       & 0xFF);
    avcC.insert(avcC.end(), sps, sps + spsLen);
    avcC.push_back(0x01);             // numOfPPS=1
    avcC.push_back((ppsLen >> 8) & 0xFF);
    avcC.push_back( ppsLen       & 0xFF);
    avcC.insert(avcC.end(), pps, pps + ppsLen);

    CLOG_INF("avcC built: profile=0x%02x level=0x%02x spsLen=%u ppsLen=%u total=%zu\n",
             sps[1], sps[3], spsLen, ppsLen, avcC.size());
    return true;
}

int C_Fmp4Muxer::InputH264(const uint8_t* data, unsigned int dataLen,
                           int64_t ptsUs, bool isKey)
{
    if (!m_bReady) return -1;
    std::lock_guard<std::mutex> lk(m_muxMu);

    // Annex-B → AVCC
    unsigned int avccLen = AnnexBToAvcc(data, dataLen, m_avccBuf);
    if (avccLen == 0) return -1;

    // 锚定首帧 PTS：第一个进入 muxer 的 packet（不论视频还是音频）作为 0 点
    if (m_firstPtsUs == AV_NOPTS_VALUE) {
        m_firstPtsUs = ptsUs;
        CLOG_INF("fmp4Muxer first PTS anchor (video): %lld us\n", (long long)ptsUs);
    }
    int64_t relUs = ptsUs - m_firstPtsUs;
    if (relUs < 0) relUs = 0;

    // us → 90kHz
    int64_t pts90k = av_rescale_q(relUs, AVRational{1, 1000000}, kTbVideo90k);
    // duration：用上一帧 PTS 估算，首帧给 1/30s
    int64_t dur90k = 90000 / 30;
    if (m_lastVideoPtsUs != AV_NOPTS_VALUE && ptsUs > m_lastVideoPtsUs) {
        dur90k = av_rescale_q(ptsUs - m_lastVideoPtsUs,
                              AVRational{1, 1000000}, kTbVideo90k);
    }
    m_lastVideoPtsUs = ptsUs;

    AVPacket* pkt = av_packet_alloc();
    pkt->data         = m_avccBuf.data();
    pkt->size         = (int)avccLen;
    pkt->stream_index = m_vStream->index;
    pkt->pts          = pts90k;
    pkt->dts          = pts90k;
    pkt->flags        = isKey ? AV_PKT_FLAG_KEY : 0;
    pkt->duration     = dur90k;

    // 必须用 interleaved：单写 av_write_frame 在 video(90kHz)+audio(SR) 双流场景下，
    // mp4 muxer 内部跨流统计 dts 会出现 "Application provided duration: -N /
    // timestamp: ... is out of range" 警告，长期累积后破坏内部状态最终 segfault
    int ret = av_interleaved_write_frame(m_fmtCtx, pkt);
    av_packet_free(&pkt);
    if (ret < 0) { MUX_PRINT_ERROR(ret); return -1; }

    static int s_vCnt = 0;
    ++s_vCnt;
    if (s_vCnt <= 3 || (s_vCnt % 150) == 0) {
        CLOG_INF("fmp4Muxer InputH264 #%d pts90k=%lld dur=%lld key=%d accum=%zu\n",
                 s_vCnt, (long long)pts90k, (long long)dur90k, isKey ? 1 : 0,
                 m_accumBuf.size());
    }

    // 触发 fragment 边界（每个 keyframe 切，所以 keyframe 这次写完肯定攒了上一段 fragment）
    avio_flush(m_ioCtx);
    FlushBufferAsFragment();
    return 0;
}

int C_Fmp4Muxer::InputAac(const uint8_t* data, unsigned int dataLen,
                          int64_t ptsUs)
{
    if (!m_bReady) return -1;
    std::lock_guard<std::mutex> lk(m_muxMu);

    // 锚定首帧 PTS：第一个进入 muxer 的 packet 作为 0 点
    // （正常 terminal 里音频在视频首 IDR 之前会被丢，所以锚定通常发生在视频，但兜底）
    if (m_firstPtsUs == AV_NOPTS_VALUE) {
        m_firstPtsUs = ptsUs;
        CLOG_INF("fmp4Muxer first PTS anchor (audio): %lld us\n", (long long)ptsUs);
    }
    int64_t relUs = ptsUs - m_firstPtsUs;
    if (relUs < 0) relUs = 0;

    // us → samples（采样率为 timebase）
    int64_t ptsSmp = av_rescale_q(relUs, AVRational{1, 1000000},
                                  AVRational{1, m_audioSampleRate});

    AVPacket* pkt = av_packet_alloc();
    pkt->data         = const_cast<uint8_t*>(data);
    pkt->size         = (int)dataLen;
    pkt->stream_index = m_aStream->index;
    pkt->pts          = ptsSmp;
    pkt->dts          = ptsSmp;
    pkt->flags        = AV_PKT_FLAG_KEY;
    pkt->duration     = 1024;   // AAC LC 1024 samples/frame

    int ret = av_interleaved_write_frame(m_fmtCtx, pkt);
    av_packet_free(&pkt);
    if (ret < 0) { MUX_PRINT_ERROR(ret); return -1; }

    static int s_aCnt = 0;
    ++s_aCnt;
    if (s_aCnt <= 3 || (s_aCnt % 250) == 0) {
        CLOG_INF("fmp4Muxer InputAac #%d ptsSmp=%lld accum=%zu\n",
                 s_aCnt, (long long)ptsSmp, m_accumBuf.size());
    }

    avio_flush(m_ioCtx);
    FlushBufferAsFragment();
    return 0;
}
