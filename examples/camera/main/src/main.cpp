#if 1
#include<stdio.h>
#include<iostream>
#include <chrono>
#include <thread>
#include <signal.h>
#include <string>
#include <unistd.h>
#include <exception>
#include <cstdlib>

#include "libmaix_image.h"
#include "libmaix_cam.h"
#include "libmaix_disp.h"

#include "libmaix_cv_image.h"

#include "terminal.h"
#include "logAdapt.h"
#include "appConfig.h"

#include "opencv2/imgproc.hpp"

#define CALC_FPS(tips)                                                                                     \
  {                                                                                                        \
    static int fcnt = 0;                                                                                   \
    fcnt++;                                                                                                \
    static struct timespec ts1, ts2;                                                                       \
    clock_gettime(CLOCK_MONOTONIC, &ts2);                                                                  \
    if ((ts2.tv_sec * 1000 + ts2.tv_nsec / 1000000) - (ts1.tv_sec * 1000 + ts1.tv_nsec / 1000000) >= 1000) \
    {                                                                                                      \
      printf("%s => H26X FPS:%d\n", tips, fcnt);                                                  \
      ts1 = ts2;                                                                                           \
      fcnt = 0;                                                                                            \
    }                                                                                                      \
  }

//摄像头采集出来的图像的分辨率，可以修改，但必须保证为16的整数倍
constexpr int kCamInW = 640;
constexpr int kCamInH = 480;

//m2dock显示屏的分辨率固定为240*240
constexpr int kDispW = 240;
constexpr int kDispH = 240;


// static void app_handlesig(int signo)
// {
//   if (SIGINT == signo || SIGTSTP == signo || SIGTERM == signo || SIGQUIT == signo || SIGPIPE == signo || SIGKILL == signo)
//   {
//     g_apprun = false;
//   }
// }

bool g_apprun = true;
int main(int argc, char **argv)
{
    // ---- 第一时间忽略 SIGPIPE：极重要 ----
    // 当客户端在 SSL_write 半路关闭连接时，OpenSSL 内部的 BIO 不带 MSG_NOSIGNAL，
    // 内核会给整个进程发 SIGPIPE，默认动作是 Term —— 没有任何 dmesg / 异常 / 日志，
    // 表现就是"进程突然没了"。这是几次"快速点击回放→进程消失"的真正凶手。
    // 忽略后，SSL_write 会改为返回 -1 / errno=EPIPE，由 IoWrite 路径正常处理。
    signal(SIGPIPE, SIG_IGN);

    // ---- 致命信号留痕：下次再"无声死亡"时能立刻看出是哪个信号 ----
    // 注意：handler 只用 async-signal-safe 的接口（write、_exit），
    //   不能用 printf / CLOG_INF / std::exception。
    auto fatalSig = [](int signo) {
        const char* name = "UNKNOWN";
        switch (signo) {
            case SIGSEGV: name = "SIGSEGV"; break;
            case SIGBUS:  name = "SIGBUS";  break;
            case SIGABRT: name = "SIGABRT"; break;
            case SIGFPE:  name = "SIGFPE";  break;
            case SIGILL:  name = "SIGILL";  break;
        }
        char buf[128];
        int n = snprintf(buf, sizeof(buf), "\n[FATAL_SIG] caught %s, aborting\n", name);
        if (n > 0) {
            ssize_t _ = write(STDERR_FILENO, buf, (size_t)n);
            (void)_;
        }
        // 恢复默认 handler 后再次 raise，让内核生成 core（如开了 ulimit -c）
        signal(signo, SIG_DFL);
        raise(signo);
    };
    signal(SIGSEGV, fatalSig);
    signal(SIGBUS,  fatalSig);
    signal(SIGABRT, fatalSig);
    signal(SIGFPE,  fatalSig);
    signal(SIGILL,  fatalSig);

    // 兜底：任何未捕获的异常（含 std::bad_alloc）都先记一笔再退出，
    // 否则进程会被 std::terminate 静默 abort，事后从日志完全看不到原因。
    // 这是之前两次"快速点击回放→进程消失"调查时遇到的最大障碍。
    std::set_terminate([]() {
        const char* what = "unknown";
        try {
            if (auto p = std::current_exception()) {
                std::rethrow_exception(p);
            }
        } catch (const std::bad_alloc& e) {
            what = "std::bad_alloc (OOM)";
        } catch (const std::exception& e) {
            what = e.what();
        } catch (...) {
            what = "non-std exception";
        }
        CLOG_ERR("FATAL std::terminate: %s\n", what);
        std::fflush(stderr);
        std::abort();
    });

    auto app_handlesig = [](int signo){
        // 注意：SIGPIPE 已在上面 SIG_IGN，不会进到这里
        if (SIGINT == signo || SIGTSTP == signo || SIGTERM == signo || SIGQUIT == signo || SIGKILL == signo)
        {
            g_apprun = false;
        }
    };

    CLOG_INF("main enter!\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(8000));  //启动时先等待一会让设备获取到ip地址和时间
    signal(SIGINT, app_handlesig);
    signal(SIGTERM, app_handlesig);

    // ---- 加载运行时配置（必须先于 Terminal 构造，因为录像分片时长来自 AppConfig） ----
    {
        char exePath[1024] = {0};
        ssize_t n = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
        std::string exeDir = ".";
        if (n > 0) {
            std::string ep(exePath);
            size_t slash = ep.find_last_of('/');
            if (slash != std::string::npos) exeDir = ep.substr(0, slash);
        }
        C_AppConfig::GetInst().Init(exeDir + "/config.json");
    }

    libmaix_image_module_init();
    libmaix_camera_module_init();

//两种方式进行图片采集，此处宏定义区分开,第一种方式性能较差建议使用第二种
#if 0
    //该方式性能较差不推荐使用,实测该方式只能达到8帧左右
    struct libmaix_cam*  m_camera = libmaix_cam_create(0, kCamInW, kCamInH, 1, 0);
    struct libmaix_cam*  m_camera1 = libmaix_cam_create(1, kCamInW, kCamInH, 0, 0); //创建摄像头通道1，不创建会有莫名其表的bug
    struct libmaix_disp * m_disp = libmaix_disp_create(0);
    m_camera->start_capture(m_camera);
    libmaix_image_t *image = nullptr;
    C_Terminal* pterminal = new C_Terminal(kCamInW, kCamInH);
    if (pterminal->Start() != 0) {
        CLOG_ERR("Terminal start failed\n");
        g_apprun = false;
    }
    while(g_apprun)
    {
        CALC_FPS("g_apprun");
        m_camera->capture_image(m_camera, &image);
        if(kCamInW == kDispW && kCamInH == kDispH){
            m_disp->draw_image(m_disp, image);
        }else{
            //采集分辨率和屏幕分辨率不同时进行缩放显示
            libmaix_image_t *rs = libmaix_image_create(kDispW, kDispH, LIBMAIX_IMAGE_MODE_RGB888, LIBMAIX_IMAGE_LAYOUT_HWC, NULL, true);
            if (rs) {
                libmaix_cv_image_resize(image, kDispW, kDispH, &rs);
                m_disp->draw_image(m_disp, rs);
                libmaix_image_destroy(&rs);
            }
        }
        //该方式采集出来的额数据为Rgb888格式，要通过InputRgb888接口送入，InputRgb888内部还要转成nv12格式送编码
        pterminal->InputRgb888((unsigned char*)image->data);
        //std::cout << "tmp->data:;" << std::hex << tmp->data << std::endl;
    }
    delete pterminal;
    libmaix_cam_destroy(&m_camera);
    libmaix_cam_destroy(&m_camera1);
    libmaix_disp_destroy(&m_disp);

#else
    //该例程能达到30帧效果
    // V831 ISP 对 cam0/cam1 创建顺序非常敏感：必须先 cam0、再 cam1，且中间不能穿插
    // 其他模块构造（vo / Terminal / TLS / HTTP API …），否则 cam0 输出会变成绿屏。
    // 因此这里把两路 cam create + start_capture 紧挨在一起，再去构造 vo / Terminal。
    // cam1（AI 专用 224×224 RGB888，不翻转）随后通过构造参数注入 PersonDetector，
    // PersonDetector 只"使用"该 cam，本作用域负责 destroy。
    struct libmaix_cam*  m_camera  = libmaix_cam_create(0, kCamInW, kCamInH, 1, 0);
    m_camera->start_capture(m_camera);
    struct libmaix_cam*  m_camera1 = libmaix_cam_create(1, 224, 224, 0, 0);
    if (m_camera1) {
        if (m_camera1->start_capture(m_camera1) != LIBMAIX_ERR_NONE) {
            CLOG_INF("cam1 start_capture failed, AI 推理将被禁用\n");
            libmaix_cam_destroy(&m_camera1);
            m_camera1 = nullptr;
        }
    } else {
        CLOG_INF("cam1 create failed, AI 推理将被禁用\n");
    }

    //输入要设置为摄像头采集的分辨率，输出在M2dock开发板上分辨率为240*240，要设置输出为240*240才能正常显示
    struct libmaix_vo * m_vo = libmaix_vo_create(kCamInW, kCamInH, 0, 0, kDispW, kDispH);

    //创建终端用于视频编码以及网络传输给客户端；cam1 注入给内部的 PersonDetector
    C_Terminal* pterminal = new C_Terminal(kCamInW, kCamInH, m_camera1);
    if (pterminal->Start() != 0) {
        CLOG_ERR("Terminal start failed\n");
        g_apprun = false;
    }
    while(g_apprun)
    {
        CALC_FPS("g_apprun");
        //std::cout << "g_apprun " << i++ << std::endl;
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
        //获取一帧数据,打印frame发现其为三个地址循环使用，libmaix_vo内部应该有三个缓冲区存放数据，所以frame内部不需要上层管理释放
        void *frame = m_vo->get_frame(m_vo, 0);
        //printf("frame addr %p\n", frame);
        if(frame == nullptr){ 
            CLOG_INF("frame == nullptr\n");
            continue;
        }
        //获取一帧数据存放的内存地址
        unsigned int *phy = NULL, *vir = NULL;
        m_vo->frame_addr(m_vo, frame, &vir, &phy);

        //摄像头直接将数据采集到对应的内存位置
        libmaix_err_t reterr = m_camera->capture(m_camera, (unsigned char *)vir[0]);
        if(reterr != LIBMAIX_ERR_NONE){ 
            CLOG_INF("reterr != LIBMAIX_ERR_NONE\n");
        }

        //根据 AppConfig 决定是否叠加 IP/时间（web 设置页可实时切换）
        cv::Mat gray(kCamInH, kCamInW, CV_8UC1, (unsigned char *)vir[0]);
        const auto osdCfg = C_AppConfig::GetInst().GetSnapshot();
        if (osdCfg.osd_show_ip) {
            cv::putText(gray, C_Terminal::get_ipv4_address().c_str(), cv::Point(5, 30), cv::FONT_HERSHEY_SIMPLEX, 1.2, cv::Scalar(255), 2);
        }
        if (osdCfg.osd_show_time) {
            std::string strData = C_LogAdapt::GetCurrentDateTimeInChina(true);
            cv::putText(gray, strData.c_str(), cv::Point(5, kCamInH-5), cv::FONT_HERSHEY_SIMPLEX, 1.2, cv::Scalar(255), 2);
        }

        // 在 cam0 的画面上叠加 cam1 推理出的人形检测框。
        //
        // 坐标转换原理：
        //   - PersonDetector 返回的 box 已是归一化 (xc, yc, w, h)，相对于 cam1 的 224×224
        //     输入空间。但 yolo2 输出"归一化坐标"本质是相对于 sensor 的视场，与具体分辨率
        //     无关 —— 只要 cam0 / cam1 共用一个 sensor 视场（V831 双 cam ISP 通道是这样的），
        //     就可以直接 *kCamInW / *kCamInH 得到 cam0 像素坐标，无需任何畸变/裁剪修正。
        //   - 当前我们只在 NV21 的 Y 平面上画白色矩形/文字，不动 UV → 颜色保持原样，
        //     代价只有几次 CPU 循环（box 数量极少），不会拖累 FPS。
        if (osdCfg.ai_enabled && osdCfg.osd_show_ai_box) {
            // 不要让旧框停留太久：仅取 1s 内的新结果
            auto boxes = pterminal->GetLatestAiBoxes(1000);
            for (const auto& b : boxes) {
                int x1 = (int)((b.xc - b.w * 0.5f) * kCamInW);
                int y1 = (int)((b.yc - b.h * 0.5f) * kCamInH);
                int x2 = (int)((b.xc + b.w * 0.5f) * kCamInW);
                int y2 = (int)((b.yc + b.h * 0.5f) * kCamInH);
                if (x1 < 0) x1 = 0; if (y1 < 0) y1 = 0;
                if (x2 > kCamInW - 1) x2 = kCamInW - 1;
                if (y2 > kCamInH - 1) y2 = kCamInH - 1;
                if (x2 <= x1 || y2 <= y1) continue;
                cv::rectangle(gray, cv::Point(x1, y1), cv::Point(x2, y2), cv::Scalar(255), 2);
                char buf[32];
                std::snprintf(buf, sizeof(buf), "person %.0f%%", b.prob * 100.0f);
                int ty = (y1 - 6 < 18) ? (y1 + 22) : (y1 - 6);   // 太靠上时把标签放框内
                cv::putText(gray, buf, cv::Point(x1 + 2, ty),
                            cv::FONT_HERSHEY_SIMPLEX, 0.8, cv::Scalar(255), 2);
            }
        }

        //YUV图片输出到屏幕上显示
        m_vo->set_frame(m_vo, frame, 0);
        //YUV图片通过编码后通过网络发送给客户端，摄像头直接采集的为nv12格式的YUV图片
        pterminal->InputNv21((unsigned char*)vir[0]);
    }

    // 必须先 delete terminal —— 它的析构会 Stop PersonDetector 线程，确保后续
    // libmaix_cam_destroy(cam1) 时不再有线程在 capture_image。
    delete pterminal;
    libmaix_vo_destroy(&m_vo);
    libmaix_cam_destroy(&m_camera);
    if (m_camera1) libmaix_cam_destroy(&m_camera1);

#endif

    libmaix_camera_module_deinit();
    libmaix_image_module_deinit();
    CLOG_INF("main end!\n");
    return 0;

}
#else

//下方为测试代码部分

#include <alsa/asoundlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include "rtpBase.h"
#define BUFFER_SIZE 1920  // 48000 Hz * 2 bytes/sample * 0.020 seconds = 1920 bytes
#define PERIOD_SIZE 960  // 48000 Hz * 0.020 seconds

void check_error(int err, const char *msg) {
    if (err < 0) {
        fprintf(stderr, "Error: %s - %s\n", msg, snd_strerror(err));
        exit(EXIT_FAILURE);
    }
}

// 生成正弦波数据
void generate_sine_wave(short *buffer, int buffer_size, int sample_rate, float frequency) {
    float amplitude = 0.01 * 32767;  // 幅度为 0.8 倍的 16 位最大值，避免溢出
    for (int i = 0; i < buffer_size / 2; i++) {
        // 计算正弦波的值
        float sample = amplitude * sinf((2.0f * M_PI * frequency * i) / sample_rate);
        // 将浮点数样本转换为 16 位整数
        buffer[i] = (short)sample;
    }
}

int main() {
    // 打开 PCM 设备
    snd_pcm_t *handle;
    int err = snd_pcm_open(&handle, "default", SND_PCM_STREAM_PLAYBACK, 0);
    check_error(err, "Opening PCM device");

    // 设置硬件参数
    snd_pcm_hw_params_t *params;
    snd_pcm_hw_params_alloca(&params);
    snd_pcm_hw_params_any(handle, params);

    // 设置访问模式
    err = snd_pcm_hw_params_set_access(handle, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    check_error(err, "Setting access type");

    // 设置样本格式
    err = snd_pcm_hw_params_set_format(handle, params, SND_PCM_FORMAT_S16_LE);
    check_error(err, "Setting format");

    // 设置通道数
    err = snd_pcm_hw_params_set_channels(handle, params, 1);
    check_error(err, "Setting channels");

    // 设置采样率
    unsigned int rate = 48000;
    int dir = 0;
    err = snd_pcm_hw_params_set_rate_near(handle, params, &rate, &dir);
    check_error(err, "Setting rate");
    if (rate != 48000) {
        fprintf(stderr, "Warning: Sample rate is not 48000 Hz, it is %d Hz\n", rate);
    }


    unsigned int buffer_time = 60 * 1000; //设置60ms缓冲区
    // 设置缓冲区时间
    err = snd_pcm_hw_params_set_buffer_time_near(handle, params, &buffer_time, &dir);
    check_error(err, "Setting buffer time");

    // 设置周期大小
    unsigned long period_size = PERIOD_SIZE;
    err = snd_pcm_hw_params_set_period_size_near(handle, params, &period_size, &dir);
    check_error(err, "Setting period size");

    // 应用硬件参数
    err = snd_pcm_hw_params(handle, params);
    check_error(err, "Applying hardware parameters");

    // 准备 PCM 设备
    err = snd_pcm_prepare(handle);
    check_error(err, "Preparing PCM device");

    // 创建缓冲区
    short buffer[BUFFER_SIZE / 2];  // 2 bytes per sample

    // 生成正弦波数据（例如 440 Hz，这是 A4 音）
    float frequency = 440.0;  // 440 Hz 是标准的 A4 音
    generate_sine_wave(buffer, BUFFER_SIZE, rate, frequency);

    int starttime = Base_GetTimeTickMs();
    int count = 0;
    // 循环播放数据
    while (true) {
        printf("time:%d  count:%d\n", Base_GetTimeTickMs() - starttime, count++);
        err = snd_pcm_writei(handle, buffer, PERIOD_SIZE);
        if (err == -EPIPE) {
            // 欠载处理
            fprintf(stderr, "Underrun occurred\n");
            snd_pcm_prepare(handle);
        } else if (err < 0) {
            check_error(err, "Writing to PCM device");
        }
    }

    // 关闭 PCM 设备
    snd_pcm_close(handle);

    return 0;
}

#endif
