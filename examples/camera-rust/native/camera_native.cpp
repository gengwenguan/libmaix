#include "camera_native.h"

#include "aacEnc.h"
#include "appConfig.h"
#include "fmp4Muxer.h"
#include "h264Enc.h"
#include "libmaix_cam.h"
#include "libmaix_image.h"
#include "logAdapt.h"
#include "personDetector.h"
#include "talkPlayer.h"
#include "tlsContext.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(__arm__)
#ifndef __NR_getrandom
#define __NR_getrandom 384
#endif
extern "C" ssize_t getrandom(void* output, size_t len, unsigned int flags)
{
    const long result = syscall(__NR_getrandom, output, len, flags);
    if (result >= 0 || errno != ENOSYS) return static_cast<ssize_t>(result);

    const int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return -1;
    size_t offset = 0;
    while (offset < len) {
        const ssize_t count = read(
            fd, static_cast<uint8_t*>(output) + offset, len - offset);
        if (count > 0) {
            offset += static_cast<size_t>(count);
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            const int saved = errno;
            close(fd);
            errno = saved;
            return -1;
        }
    }
    close(fd);
    return static_cast<ssize_t>(offset);
}
#endif

extern "C" {
struct OpusEncoder;
OpusEncoder* opus_encoder_create(
    int sample_rate, int channels, int application, int* error);
int opus_encode(
    OpusEncoder* encoder, const int16_t* pcm, int frame_size,
    unsigned char* output, int max_output_bytes);
int opus_encoder_ctl(OpusEncoder* encoder, int request, ...);
void opus_encoder_destroy(OpusEncoder* encoder);
}

namespace {

constexpr int kWidth = 640;
constexpr int kHeight = 480;
constexpr int kDisplayWidth = 240;
constexpr int kDisplayHeight = 240;
constexpr int kOpusApplicationVoip = 2048;
constexpr int kOpusSetBitrateRequest = 4002;
constexpr int kOpusSetComplexityRequest = 4010;
constexpr int kOpusFrameSamples = 960;
constexpr int kOpusMaxPacketBytes = 4000;

std::string GetIpv4Address()
{
    struct ifaddrs* interfaces = nullptr;
    if (getifaddrs(&interfaces) != 0) return "0.0.0.0";

    std::string result = "0.0.0.0";
    for (struct ifaddrs* it = interfaces; it; it = it->ifa_next) {
        if (!it->ifa_addr || it->ifa_addr->sa_family != AF_INET) continue;
        if ((it->ifa_flags & IFF_LOOPBACK) != 0) continue;
        char text[INET_ADDRSTRLEN] = {0};
        const auto* in = reinterpret_cast<const struct sockaddr_in*>(it->ifa_addr);
        if (inet_ntop(AF_INET, &in->sin_addr, text, sizeof(text))) {
            result = text;
            break;
        }
    }
    freeifaddrs(interfaces);
    return result;
}

int AacFrequencyIndex(int sampleRate)
{
    static const int table[] = {
        96000, 88200, 64000, 48000, 44100, 32000, 24000,
        22050, 16000, 12000, 11025, 8000, 7350,
    };
    for (int i = 0; i < 13; ++i) {
        if (table[i] == sampleRate) return i;
    }
    return 3;
}

void BuildAdts(const unsigned char* aac, unsigned int aacLen,
               int sampleRate, int channels, std::vector<unsigned char>& out)
{
    const unsigned int frameLen = aacLen + 7;
    const int profile = 1;
    const int frequency = AacFrequencyIndex(sampleRate);
    out.resize(frameLen);
    out[0] = 0xff;
    out[1] = 0xf1;
    out[2] = static_cast<unsigned char>((profile << 6) |
                                        (frequency << 2) |
                                        (channels >> 2));
    out[3] = static_cast<unsigned char>(((channels & 3) << 6) |
                                        ((frameLen >> 11) & 3));
    out[4] = static_cast<unsigned char>((frameLen >> 3) & 0xff);
    out[5] = static_cast<unsigned char>(((frameLen & 7) << 5) | 0x1f);
    out[6] = 0xfc;
    std::memcpy(out.data() + 7, aac, aacLen);
}

class CallbackLogSink : public ILogSink {
public:
    explicit CallbackLogSink(const camera_native_callbacks& callbacks)
        : callbacks_(callbacks)
    {}

    void OnLogLine(const char* line, unsigned int len) override
    {
        if (callbacks_.on_log_line && line && len > 0) {
            callbacks_.on_log_line(
                callbacks_.opaque,
                reinterpret_cast<const uint8_t*>(line),
                static_cast<size_t>(len));
        }
    }

private:
    camera_native_callbacks callbacks_;
};

class CameraEngine final : public C_H264Enc::C_Listener,
                           public C_AacEnc::C_Listener,
                           public C_Fmp4Muxer::C_Listener {
public:
    explicit CameraEngine(const camera_native_callbacks& callbacks)
        : callbacks_(callbacks),
          logSink_(callbacks)
    {}

    ~CameraEngine() override { Stop(); }

    int Start()
    {
        bool expected = false;
        if (!running_.compare_exchange_strong(expected, true)) return 0;

        SetLogSink(&logSink_);
        try {
            std::fprintf(stderr, "[native-stage] image module init\n");
            std::fflush(stderr);
            libmaix_image_module_init();
            std::fprintf(stderr, "[native-stage] camera module init\n");
            std::fflush(stderr);
            libmaix_camera_module_init();
            modulesInited_ = true;

            // V831 ISP requires cam0 and cam1 to be created and started
            // consecutively, before VO, VE, ALSA, TLS, or any worker thread.
            std::fprintf(stderr, "[native-stage] cam0 create/start\n");
            std::fflush(stderr);
            camera0_ = libmaix_cam_create(0, kWidth, kHeight, 1, 0);
            if (!camera0_ ||
                camera0_->start_capture(camera0_) != LIBMAIX_ERR_NONE) {
                return FailStart("cam0 create/start failed");
            }
            std::fprintf(stderr, "[native-stage] cam1 create/start\n");
            std::fflush(stderr);
            camera1_ = libmaix_cam_create(1, 224, 224, 0, 0);
            if (camera1_ &&
                camera1_->start_capture(camera1_) != LIBMAIX_ERR_NONE) {
                libmaix_cam_destroy(&camera1_);
                camera1_ = nullptr;
                CLOG_WRN("native bridge: cam1 unavailable, AI disabled\n");
            }

            std::fprintf(stderr, "[native-stage] VO create\n");
            std::fflush(stderr);
            vo_ = libmaix_vo_create(
                kWidth, kHeight, 0, 0, kDisplayWidth, kDisplayHeight);
            if (!vo_) return FailStart("VO create failed");

            std::fprintf(stderr, "[native-stage] media modules create\n");
            std::fflush(stderr);
            person_.reset(new C_PersonDetector(kWidth, kHeight));
            {
                const auto config = C_AppConfig::GetInst().GetSnapshot();
                person_->SetExternalConfig(
                    config.ai_enabled, config.ai_threshold, config.ai_infer_fps);
            }
            person_->SetDetectionCallback([this](float probability) {
                if (callbacks_.on_person_detected) {
                    callbacks_.on_person_detected(callbacks_.opaque, probability);
                }
            });
            if (camera1_) {
                person_->SetAiCam(camera1_);
                if (person_->Start("/root/models") != 0) {
                    CLOG_WRN("native bridge: person detector unavailable\n");
                }
            }

            h264_.reset(new C_H264Enc(this, kWidth, kHeight, kWidth, kHeight));
            aac_.reset(new C_AacEnc(this));
            talk_.reset(new C_TalkPlayer());

            if (aac_->Start() != 0) return FailStart("AAC start failed");

            captureThread_ = std::thread(&CameraEngine::CaptureLoop, this);
            std::fprintf(stderr, "[native-stage] capture thread started\n");
            std::fflush(stderr);
            CLOG_INF("native bridge: media engine started\n");
            return 0;
        } catch (const std::exception& error) {
            return FailStart(std::string("native exception: ") + error.what());
        } catch (...) {
            return FailStart("unknown native exception");
        }
    }

    void Stop()
    {
        const bool wasRunning = running_.exchange(false);
        if (captureThread_.joinable()) captureThread_.join();
        if (!wasRunning && !modulesInited_) return;

        SetLogSink(nullptr);
        if (aac_) aac_->Stop();
        if (person_) person_->Stop();
        if (talk_) talk_->Shutdown();
        {
            std::lock_guard<std::mutex> lock(opusMutex_);
            if (opusEncoder_) {
                opus_encoder_destroy(opusEncoder_);
                opusEncoder_ = nullptr;
            }
        }

        {
            std::lock_guard<std::mutex> lock(muxerMutex_);
            muxer_.reset();
        }
        talk_.reset();
        person_.reset();
        aac_.reset();
        h264_.reset();

        if (vo_) libmaix_vo_destroy(&vo_);
        if (camera0_) libmaix_cam_destroy(&camera0_);
        if (camera1_) libmaix_cam_destroy(&camera1_);
        if (modulesInited_) {
            libmaix_camera_module_deinit();
            libmaix_image_module_deinit();
            modulesInited_ = false;
        }
    }

    void ForceIframe()
    {
        if (h264_) h264_->ForceIframe();
    }

    void SetWebRtcActive(bool active)
    {
        webrtcActive_.store(active, std::memory_order_release);
        if (active) {
            ForceIframe();
            return;
        }
        ReleaseOpusIfIdle();
    }

    void SetRemoteVideoActive(bool active)
    {
        remoteVideoActive_.store(active, std::memory_order_release);
        if (active) ForceIframe();
    }

    void ReleaseOpusIfIdle()
    {
        if (webrtcActive_.load(std::memory_order_acquire)) return;
        std::lock_guard<std::mutex> lock(opusMutex_);
        if (opusEncoder_) {
            opus_encoder_destroy(opusEncoder_);
            opusEncoder_ = nullptr;
        }
    }

    int SetConfig(const camera_native_runtime_config& config)
    {
        C_AppConfig::Snapshot snapshot;
        snapshot.ai_enabled = config.ai_enabled != 0;
        snapshot.ai_threshold = config.ai_threshold;
        snapshot.ai_min_interval_s = config.ai_min_interval_s;
        snapshot.ai_infer_fps = config.ai_infer_fps;
        snapshot.album_max_photos = config.album_max_photos;
        snapshot.photo_jpeg_qual = config.photo_jpeg_qual;
        snapshot.mic_filter_mode = config.mic_filter_mode;
        snapshot.osd_show_ip = config.osd_show_ip != 0;
        snapshot.osd_show_time = config.osd_show_time != 0;
        snapshot.osd_show_ai_box = config.osd_show_ai_box != 0;
        C_AppConfig::GetInst().SetSnapshot(snapshot);
        if (person_) {
            person_->SetExternalConfig(
                snapshot.ai_enabled, snapshot.ai_threshold, snapshot.ai_infer_fps);
        }
        return 0;
    }

    bool EncodeJpeg(const uint8_t* nv21, size_t len, int quality,
                    std::vector<uint8_t>& jpeg)
    {
        const size_t expected = static_cast<size_t>(kWidth) * kHeight * 3 / 2;
        if (!nv21 || len < expected) return false;
        cv::Mat source(
            kHeight * 3 / 2, kWidth, CV_8UC1,
            const_cast<uint8_t*>(nv21));
        cv::Mat bgr;
        cv::cvtColor(source, bgr, cv::COLOR_YUV2BGR_NV21);
        quality = std::max(30, std::min(100, quality));
        std::vector<int> parameters{cv::IMWRITE_JPEG_QUALITY, quality};
        return cv::imencode(".jpg", bgr, jpeg, parameters) && !jpeg.empty();
    }

    int TalkOpen() { return talk_ && talk_->InitDevice() ? 0 : -1; }
    void TalkClose()
    {
        if (talk_) {
            talk_->Reset();
            talk_->Shutdown();
        }
    }
    int TalkFeed(const uint8_t* data, size_t len)
    {
        if (!talk_ || len > 0xffffffffu) return -1;
        return talk_->DecodePlayOpus(data, static_cast<unsigned int>(len));
    }
    int PlayPromptPcm(const int16_t* samples, size_t frames)
    {
        if (!talk_ || !samples || frames == 0 ||
            frames > static_cast<size_t>(0xffffffffu)) return -1;
        return talk_->PlayPcmSync(
            reinterpret_cast<const short*>(samples),
            static_cast<unsigned long>(frames));
    }

    int MicLoudness() const { return AacEnc_GetMicLoudness(); }
    const std::string& LastError() const { return lastError_; }

    int OnOutputH264(unsigned char* data, unsigned int len,
                     int64_t ptsUs, bool isKey) override
    {
        if ((webrtcActive_.load(std::memory_order_acquire) ||
             remoteVideoActive_.load(std::memory_order_acquire)) &&
            callbacks_.on_h264_access_unit && data && len > 0) {
            if (isKey && h264_) {
                unsigned int extraLen = 0;
                const unsigned char* extra = h264_->GetSpsPps(&extraLen);
                if (extra && extraLen > 0) {
                    std::vector<uint8_t> accessUnit;
                    accessUnit.reserve(static_cast<size_t>(extraLen) + len);
                    accessUnit.insert(
                        accessUnit.end(), extra, extra + extraLen);
                    accessUnit.insert(accessUnit.end(), data, data + len);
                    callbacks_.on_h264_access_unit(
                        callbacks_.opaque, accessUnit.data(), accessUnit.size(),
                        ptsUs, 1);
                } else {
                    callbacks_.on_h264_access_unit(
                        callbacks_.opaque, data, len, ptsUs, 1);
                }
            } else {
                callbacks_.on_h264_access_unit(
                    callbacks_.opaque, data, len, ptsUs, isKey ? 1 : 0);
            }
        }
        auto muxer = GetOrCreateMuxer();
        if (muxer) muxer->InputH264(data, len, ptsUs, isKey);
        return 0;
    }

    int OnOutputPcm(
        const int16_t* samples, unsigned int frames, int64_t ptsUs) override
    {
        if (!webrtcActive_.load(std::memory_order_acquire) ||
            !callbacks_.on_opus_frame || !samples ||
            frames != kOpusFrameSamples) {
            return 0;
        }

        std::lock_guard<std::mutex> lock(opusMutex_);
        if (!opusEncoder_) {
            int error = 0;
            opusEncoder_ = opus_encoder_create(
                48000, 1, kOpusApplicationVoip, &error);
            if (!opusEncoder_ || error != 0) {
                CLOG_ERR("native bridge: Opus encoder create failed: %d\n", error);
                opusEncoder_ = nullptr;
                return -1;
            }
            opus_encoder_ctl(opusEncoder_, kOpusSetBitrateRequest, 24000);
            opus_encoder_ctl(opusEncoder_, kOpusSetComplexityRequest, 3);
            opusPacket_.resize(kOpusMaxPacketBytes);
        }

        const int bytes = opus_encode(
            opusEncoder_, samples, static_cast<int>(frames),
            opusPacket_.data(), static_cast<int>(opusPacket_.size()));
        if (bytes <= 0) {
            CLOG_ERR("native bridge: Opus encode failed: %d\n", bytes);
            return -1;
        }
        callbacks_.on_opus_frame(
            callbacks_.opaque, opusPacket_.data(), static_cast<size_t>(bytes),
            ptsUs, 0);
        return 0;
    }

    int OnOutputAac(unsigned char* data, unsigned int len, int64_t ptsUs) override
    {
        if (callbacks_.on_audio_adts) {
            std::vector<unsigned char> adts;
            BuildAdts(data, len, static_cast<int>(aac_->SampleRate()),
                      static_cast<int>(aac_->Channels()), adts);
            callbacks_.on_audio_adts(
                callbacks_.opaque, adts.data(), adts.size(), ptsUs, 0);
        }
        std::shared_ptr<C_Fmp4Muxer> muxer;
        {
            std::lock_guard<std::mutex> lock(muxerMutex_);
            muxer = muxer_;
        }
        if (muxer) muxer->InputAac(data, len, ptsUs);
        return 0;
    }

    void OnInitSegment(const uint8_t* data, size_t len) override
    {
        if (callbacks_.on_init_segment) {
            callbacks_.on_init_segment(callbacks_.opaque, data, len);
        }
    }

    void OnFragment(const uint8_t* data, size_t len) override
    {
        if (callbacks_.on_fragment) {
            callbacks_.on_fragment(callbacks_.opaque, data, len);
        }
    }

private:
    int FailStart(const std::string& error)
    {
        lastError_ = error;
        CLOG_ERR("native bridge start failed: %s\n", error.c_str());
        Stop();
        return -1;
    }

    std::shared_ptr<C_Fmp4Muxer> GetOrCreateMuxer()
    {
        std::lock_guard<std::mutex> lock(muxerMutex_);
        if (muxer_) return muxer_;
        if (!h264_ || !aac_) return nullptr;

        unsigned int videoExtraLen = 0;
        const unsigned char* videoExtra = h264_->GetSpsPps(&videoExtraLen);
        unsigned int audioExtraLen = 0;
        const unsigned char* audioExtra =
            aac_->GetAudioSpecificConfig(&audioExtraLen);
        if (!videoExtra || videoExtraLen == 0 ||
            !audioExtra || audioExtraLen == 0) {
            return nullptr;
        }

        C_Fmp4Muxer::VideoConfig video{
            kWidth, kHeight, videoExtra, videoExtraLen};
        C_Fmp4Muxer::AudioConfig audio{
            static_cast<int>(aac_->SampleRate()),
            static_cast<int>(aac_->Channels()),
            audioExtra,
            audioExtraLen};
        auto candidate = std::make_shared<C_Fmp4Muxer>(this, video, audio);
        if (!candidate->IsReady()) return nullptr;
        muxer_ = candidate;
        return muxer_;
    }

    void CaptureLoop()
    {
        const std::string ip = GetIpv4Address();
        while (running_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
            void* frame = vo_->get_frame(vo_, 0);
            if (!frame) continue;

            unsigned int* physical = nullptr;
            unsigned int* virtualAddress = nullptr;
            vo_->frame_addr(vo_, frame, &virtualAddress, &physical);
            if (!virtualAddress) continue;
            auto* pixels = reinterpret_cast<unsigned char*>(
                static_cast<uintptr_t>(virtualAddress[0]));
            if (!pixels) continue;
            if (camera0_->capture(camera0_, pixels) != LIBMAIX_ERR_NONE) continue;

            const auto config = C_AppConfig::GetInst().GetSnapshot();
            cv::Mat gray(kHeight, kWidth, CV_8UC1, pixels);
            if (config.osd_show_ip) {
                cv::putText(gray, ip, cv::Point(5, 30),
                            cv::FONT_HERSHEY_SIMPLEX, 1.2, cv::Scalar(255), 2);
            }
            if (config.osd_show_time) {
                cv::putText(gray, C_LogAdapt::GetCurrentDateTimeInChina(true),
                            cv::Point(5, kHeight - 5),
                            cv::FONT_HERSHEY_SIMPLEX, 1.2, cv::Scalar(255), 2);
            }
            if (config.ai_enabled && config.osd_show_ai_box && person_) {
                for (const auto& box : person_->GetLatestBoxes(1000)) {
                    int x1 = static_cast<int>((box.xc - box.w * 0.5f) * kWidth);
                    int y1 = static_cast<int>((box.yc - box.h * 0.5f) * kHeight);
                    int x2 = static_cast<int>((box.xc + box.w * 0.5f) * kWidth);
                    int y2 = static_cast<int>((box.yc + box.h * 0.5f) * kHeight);
                    x1 = std::max(0, std::min(kWidth - 1, x1));
                    y1 = std::max(0, std::min(kHeight - 1, y1));
                    x2 = std::max(0, std::min(kWidth - 1, x2));
                    y2 = std::max(0, std::min(kHeight - 1, y2));
                    if (x2 <= x1 || y2 <= y1) continue;
                    cv::rectangle(gray, cv::Point(x1, y1), cv::Point(x2, y2),
                                  cv::Scalar(255), 2);
                }
            }

            vo_->set_frame(vo_, frame, 0);
            if (callbacks_.on_nv21_frame) {
                callbacks_.on_nv21_frame(
                    callbacks_.opaque, pixels,
                    static_cast<size_t>(kWidth) * kHeight * 3 / 2);
            }
            if (h264_) h264_->InputData(pixels);
        }
    }

    camera_native_callbacks callbacks_{};
    CallbackLogSink logSink_;
    std::string lastError_;
    std::atomic<bool> running_{false};
    bool modulesInited_ = false;

    libmaix_cam_t* camera0_ = nullptr;
    libmaix_cam_t* camera1_ = nullptr;
    libmaix_vo_t* vo_ = nullptr;
    std::thread captureThread_;

    std::unique_ptr<C_H264Enc> h264_;
    std::unique_ptr<C_AacEnc> aac_;
    std::unique_ptr<C_PersonDetector> person_;
    std::unique_ptr<C_TalkPlayer> talk_;
    std::atomic<bool> webrtcActive_{false};
    std::atomic<bool> remoteVideoActive_{false};
    std::mutex opusMutex_;
    OpusEncoder* opusEncoder_ = nullptr;
    std::vector<uint8_t> opusPacket_;
    std::mutex muxerMutex_;
    std::shared_ptr<C_Fmp4Muxer> muxer_;
};

} // namespace

struct camera_native {
    std::unique_ptr<CameraEngine> engine;
};

struct camera_tls_context {
    C_TlsContext context;
};

struct camera_tls_connection {
    std::unique_ptr<C_SslConn> connection;
};

extern "C" camera_native* camera_native_create(
    const char* runtimeDir, const camera_native_callbacks* callbacks)
{
    if (!runtimeDir || !callbacks) return nullptr;
    try {
        std::unique_ptr<camera_native> camera(new camera_native());
        camera->engine.reset(new CameraEngine(*callbacks));
        return camera.release();
    } catch (...) {
        return nullptr;
    }
}

extern "C" int camera_native_start(camera_native* camera)
{
    return camera && camera->engine ? camera->engine->Start() : -1;
}

extern "C" void camera_native_stop(camera_native* camera)
{
    if (camera && camera->engine) camera->engine->Stop();
}

extern "C" void camera_native_destroy(camera_native* camera)
{
    delete camera;
}

extern "C" void camera_native_force_iframe(camera_native* camera)
{
    if (camera && camera->engine) camera->engine->ForceIframe();
}

extern "C" void camera_native_webrtc_set_active(
    camera_native* camera, int active)
{
    if (camera && camera->engine) {
        camera->engine->SetWebRtcActive(active != 0);
    }
}

extern "C" void camera_native_remote_video_set_active(
    camera_native* camera, int active)
{
    if (camera && camera->engine) {
        camera->engine->SetRemoteVideoActive(active != 0);
    }
}

extern "C" int camera_native_config_set(
    camera_native* camera, const camera_native_runtime_config* config)
{
    return camera && camera->engine && config
        ? camera->engine->SetConfig(*config)
        : -1;
}

extern "C" int camera_native_encode_jpeg(
    camera_native* camera, const uint8_t* nv21, size_t len, int quality,
    uint8_t** output, size_t* outputLen)
{
    if (!camera || !camera->engine || !output || !outputLen) return -1;
    *output = nullptr;
    *outputLen = 0;
    std::vector<uint8_t> jpeg;
    if (!camera->engine->EncodeJpeg(nv21, len, quality, jpeg)) return -1;
    auto* bytes = static_cast<uint8_t*>(std::malloc(jpeg.size()));
    if (!bytes) return -1;
    std::memcpy(bytes, jpeg.data(), jpeg.size());
    *output = bytes;
    *outputLen = jpeg.size();
    return 0;
}

extern "C" int camera_native_talk_open(camera_native* camera)
{
    return camera && camera->engine ? camera->engine->TalkOpen() : -1;
}

extern "C" void camera_native_talk_close(camera_native* camera)
{
    if (camera && camera->engine) camera->engine->TalkClose();
}

extern "C" int camera_native_talk_feed(
    camera_native* camera, const uint8_t* data, size_t len)
{
    return camera && camera->engine
        ? camera->engine->TalkFeed(data, len)
        : -1;
}

extern "C" int camera_native_play_prompt_pcm(
    camera_native* camera, const int16_t* samples, size_t frames)
{
    return camera && camera->engine
        ? camera->engine->PlayPromptPcm(samples, frames)
        : -1;
}

extern "C" int camera_native_mic_loudness(camera_native* camera)
{
    return camera && camera->engine ? camera->engine->MicLoudness() : 0;
}

extern "C" void camera_native_free(void* ptr) { std::free(ptr); }

extern "C" const char* camera_native_last_error(camera_native* camera)
{
    return camera && camera->engine
        ? camera->engine->LastError().c_str()
        : "camera_native is null";
}

extern "C" camera_tls_context* camera_tls_context_create(
    const char* certPath, const char* keyPath)
{
    if (!certPath || !keyPath) return nullptr;
    std::unique_ptr<camera_tls_context> context(new camera_tls_context());
    if (!context->context.Init(certPath, keyPath)) return nullptr;
    return context.release();
}

extern "C" void camera_tls_context_destroy(camera_tls_context* context)
{
    delete context;
}

extern "C" camera_tls_connection* camera_tls_accept(
    camera_tls_context* context, int fd)
{
    if (!context || fd < 0) return nullptr;
    auto accepted = context->context.AcceptOnFd(fd);
    if (!accepted) return nullptr;
    std::unique_ptr<camera_tls_connection> connection(
        new camera_tls_connection());
    connection->connection = std::move(accepted);
    return connection.release();
}

extern "C" int camera_tls_read(
    camera_tls_connection* connection, void* data, int len, int* wantMore)
{
    if (!connection || !connection->connection || !data || len <= 0) return -1;
    bool want = false;
    int result = connection->connection->Read(data, len, want);
    if (wantMore) *wantMore = want ? 1 : 0;
    return result;
}

extern "C" int camera_tls_write(
    camera_tls_connection* connection, const void* data, int len, int* wantMore)
{
    if (!connection || !connection->connection || !data || len <= 0) return -1;
    bool want = false;
    int result = connection->connection->Write(data, len, want);
    if (wantMore) *wantMore = want ? 1 : 0;
    return result;
}

extern "C" void camera_tls_connection_destroy(
    camera_tls_connection* connection)
{
    if (!connection) return;
    if (connection->connection) connection->connection->Shutdown();
    delete connection;
}
