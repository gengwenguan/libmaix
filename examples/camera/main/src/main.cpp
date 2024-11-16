
#include<stdio.h>
#include<iostream>
#include <fstream>
#include <chrono>
#include <thread>
#include <signal.h>
#include <string>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>  // 包含这个头文件以确保 NI_MAXHOST 和 NI_NUMERICHOST 定义
#include "libmaix_image.h"
#include "libmaix_cam.h"
#include "libmaix_disp.h"

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

constexpr int kInW = 240;
constexpr int kInH = 240;

bool g_apprun = true;
static void app_handlesig(int signo)
{
  if (SIGINT == signo || SIGTSTP == signo || SIGTERM == signo || SIGQUIT == signo || SIGPIPE == signo || SIGKILL == signo)
  {
    g_apprun = false;
  }
}



// 获取 IPv4 地址的接口
std::string get_ipv4_address() {
    struct ifaddrs *ifaddr, *ifa;
    char host[NI_MAXHOST];
    std::string ipv4_address = "0.0.0.0";

    // 获取网络接口信息
    if (getifaddrs(&ifaddr) == -1) {
        perror("getifaddrs");
        return "";
    }

    // 遍历所有网络接口
    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL)
            continue;

        int family = ifa->ifa_addr->sa_family;

        // 只处理 IPv4 地址
        if (family == AF_INET) {
            int s = getnameinfo(ifa->ifa_addr, sizeof(struct sockaddr_in), host, NI_MAXHOST, NULL, 0, NI_NUMERICHOST);
            if (s != 0) {
                std::cerr << "getnameinfo() failed: " << gai_strerror(s) << std::endl;
                continue;
            }
            // 找到第一个 IPv4 地址并返回
            ipv4_address = host;
            if(ipv4_address == "127.0.0.1"){ continue; } //找到的为127.0.0.1本地回环地址跳过
            break;
        }
    }

    freeifaddrs(ifaddr); // 释放资源
    return ipv4_address;
}

int main(int argc, char **argv)
{
    CLOG_INF("main enter!");
    signal(SIGINT, app_handlesig);
    signal(SIGTERM, app_handlesig);

    libmaix_camera_module_init();
    libmaix_image_module_init();

//两种方式进行图片采集，此处宏定义区分开
#if 0
    struct libmaix_cam*  m_camera = libmaix_cam_create(0, kInW, kInH, 1, 0);
    struct libmaix_disp * m_disp = libmaix_disp_create(0);
    m_camera->start_capture(m_camera);
    libmaix_image_t *image = nullptr;
    C_Terminal* pterminal = new C_Terminal(kInW, kInH);
    while(g_apprun)
    {
        CALC_FPS("g_apprun");
        m_camera->capture_image(m_camera, &image);
        m_disp->draw_image(m_disp, image);
        pterminal->InputRgb888((unsigned char*)image->data);
        //m_pH264Enc->InputRgb888((unsigned char*)image->data);
        //std::cout << "tmp->data:;" << std::hex << tmp->data << std::endl;
    }
    delete pterminal;

#else

    struct libmaix_cam*  m_camera = libmaix_cam_create(0, kInW, kInH, 1, 0);
    m_camera->start_capture(m_camera);
    struct libmaix_vo * m_vo = libmaix_vo_create(kInW, kInH, 0, 0, kInW, kInH);
    C_Terminal* pterminal = new C_Terminal(kInW, kInH);
    while(g_apprun)
    {
        //CLOG_INF("g_apprun");
        CALC_FPS("g_apprun");
        //std::cout << "g_apprun " << i++ << std::endl;
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
        void *frame = m_vo->get_frame(m_vo, 0);
        if(frame == nullptr){ 
            std::cout << "frame == nullptr " << std::endl;
            continue;
        }
        unsigned int *phy = NULL, *vir = NULL;
        m_vo->frame_addr(m_vo, frame, &vir, &phy);
        libmaix_err_t reterr = m_camera->capture(m_camera, (unsigned char *)vir[0]);
        if(reterr != LIBMAIX_ERR_NONE){ 
            std::cout << "reterr != LIBMAIX_ERR_NONE" << std::endl;
        }

        //将ip地址渲染到图片上之后再进行显示
        cv::Mat gray(kInH, kInW, CV_8UC1, (unsigned char *)vir[0]);
        cv::putText(gray, get_ipv4_address().c_str(), cv::Point(5, 20), cv::FONT_HERSHEY_SIMPLEX, 0.8, cv::Scalar(255), 2);
        
        //图片输出到屏幕上
        m_vo->set_frame(m_vo, frame, 0);
        //图片通过编码后通过网络发送给客户端
        pterminal->InputNv21((unsigned char*)vir[0]);
        //CALC_FPS("g_apprun");
    }
    delete pterminal;

#endif

    libmaix_camera_module_deinit();
    libmaix_image_module_deinit();
    CLOG_INF("main end!");
    return 0;

}
