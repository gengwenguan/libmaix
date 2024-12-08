
#include<stdio.h>
#include<iostream>
#include <fstream>
#include <chrono>
#include <thread>
#include <signal.h>
#include <string>

#include "libmaix_image.h"
#include "libmaix_cam.h"
#include "libmaix_disp.h"

#include "libmaix_cv_image.h"

#include "terminal.h"
#include "logAdapt.h"

#include "opencv2/core.hpp"
#include "opencv2/imgproc.hpp"
#include "opencv2/highgui.hpp"
#include "opencv2/videoio.hpp"
#include "opencv2/imgcodecs.hpp"
#include <opencv2/opencv.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs/legacy/constants_c.h>
#include "opencv2/core/types_c.h"

#include <alsa/asoundlib.h>

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

bool g_apprun = true;
static void app_handlesig(int signo)
{
  if (SIGINT == signo || SIGTSTP == signo || SIGTERM == signo || SIGQUIT == signo || SIGPIPE == signo || SIGKILL == signo)
  {
    g_apprun = false;
  }
}


int main(int argc, char **argv)
{
    CLOG_INF("main enter!\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(8000));  //启动时先等待一会让设备获取到ip地址和时间
    signal(SIGINT, app_handlesig);
    signal(SIGTERM, app_handlesig);

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
    struct libmaix_cam*  m_camera = libmaix_cam_create(0, kCamInW, kCamInH, 1, 0);
    struct libmaix_cam*  m_camera1 = libmaix_cam_create(1, kCamInW, kCamInH, 0, 0); //创建摄像头通道1，不创建会有莫名其表的bug
    m_camera->start_capture(m_camera);

    //输入要设置为摄像头采集的分辨率，输出在M2dock开发板上分辨率为240*240，要设置输出为240*240才能正常显示
    struct libmaix_vo * m_vo = libmaix_vo_create(kCamInW, kCamInH, 0, 0, kDispW, kDispH);

    //创建终端用于视频编码以及网络传输给客户端
    C_Terminal* pterminal = new C_Terminal(kCamInW, kCamInH);
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

        //将该设备的ip地址渲染到图片最上方
        cv::Mat gray(kCamInH, kCamInW, CV_8UC1, (unsigned char *)vir[0]);
        cv::putText(gray, C_Terminal::get_ipv4_address().c_str(), cv::Point(5, 30), cv::FONT_HERSHEY_SIMPLEX, 1.2, cv::Scalar(255), 2);
        //将日期时间渲染到图片最下方
        std::string strData = C_LogAdapt::GetCurrentDateTimeInChina(true);
        cv::putText(gray, strData.c_str(), cv::Point(5, kCamInH-5), cv::FONT_HERSHEY_SIMPLEX, 1.2, cv::Scalar(255), 2);

        //YUV图片输出到屏幕上显示
        m_vo->set_frame(m_vo, frame, 0);
        //YUV图片通过编码后通过网络发送给客户端，摄像头直接采集的为nv12格式的YUV图片
        pterminal->InputNv21((unsigned char*)vir[0]);
    }

    libmaix_vo_destroy(&m_vo);
    libmaix_cam_destroy(&m_camera);
    libmaix_cam_destroy(&m_camera1);
    delete pterminal;

    // int err;
    // snd_pcm_t *capture_handle;// 一个指向PCM设备的句柄

	// if ((err = snd_pcm_open (&capture_handle, argv[1],SND_PCM_STREAM_CAPTURE,0))<0) 
	// {
	// 	printf("无法打开音频设备: %s (%s)\n",  argv[1],snd_strerror (err));
	// 	exit(1);
	// }

#endif

    libmaix_camera_module_deinit();
    libmaix_image_module_deinit();
    CLOG_INF("main end!\n");
    return 0;

}
