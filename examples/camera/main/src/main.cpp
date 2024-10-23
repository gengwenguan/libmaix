
#include<stdio.h>
#include<iostream>
#include <fstream>
#include <chrono>
#include <thread>
#include <signal.h>
#include "libmaix_image.h"
#include "libmaix_cam.h"
#include "libmaix_disp.h"

#include "terminal.h"

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

constexpr int inW = 240;
constexpr int inH = 240;

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
    signal(SIGINT, app_handlesig);
    signal(SIGTERM, app_handlesig);
    
    // struct libmaix_cam*  m_camera = libmaix_cam_create(0, inW, inH, 1, 0);
    // m_camera->start_capture(m_camera);
    // struct libmaix_vo * m_vo = libmaix_vo_create(inW, inH, 0, 0, inW, inH);

    // for(int i=0; i<60; i++)
    // {
    //     std::this_thread::sleep_for(std::chrono::milliseconds(30));
    //     void *frame = m_vo->get_frame(m_vo, 0);
    //     if(frame == nullptr){ 
    //         std::cout << "frame == nullptr i=" << i << std::endl;
    //         continue;
    //     }
    //     unsigned int *phy = NULL, *vir = NULL;
    //     m_vo->frame_addr(m_vo, frame, &vir, &phy);
    //     libmaix_err_t reterr = m_camera->capture(m_camera, (unsigned char *)vir[0]);
    //     if(reterr != LIBMAIX_ERR_NONE){ 
    //         std::cout << "reterr != LIBMAIX_ERR_NONE" << std::endl;
    //     }
    //     m_vo->set_frame(m_vo, frame, 0);
    //     if(i == 59){
    //         // 创建一个输出文件流对象
    //         std::ofstream outfileRGB("RGB.data", std::ios::binary);
    //         // 检查文件是否成功打开
    //         if (!outfileRGB.is_open()) {
    //             std::cerr << "无法打开文件 " << "RGB.data" << " 进行写入！" << std::endl;
    //             return 1;
    //         }
    //         outfileRGB.write((const char *)vir[0], inW*inH*3);
    //         outfileRGB.close();
    //     }

    // }

    libmaix_camera_module_init();
    libmaix_image_module_init();

    
    // struct libmaix_cam*  m_camera = libmaix_cam_create(0, inW, inH, 1, 0);
    // struct libmaix_disp * m_disp = libmaix_disp_create(0);
    // m_camera->start_capture(m_camera);
    // libmaix_image_t *image = nullptr;
    // C_Terminal* pterminal = new C_Terminal(inW, inH);
    // while(g_apprun)
    // {
    //     m_camera->capture_image(m_camera, &image);
    //     m_disp->draw_image(m_disp, image);
    //     pterminal->InputRgb888((unsigned char*)image->data);
    //     //m_pH264Enc->InputRgb888((unsigned char*)image->data);
    //     //std::cout << "tmp->data:;" << std::hex << tmp->data << std::endl;
    // }
    // delete pterminal;

    struct libmaix_cam*  m_camera = libmaix_cam_create(0, inW, inH, 1, 0);
    m_camera->start_capture(m_camera);
    struct libmaix_vo * m_vo = libmaix_vo_create(inW, inH, 0, 0, inW, inH);
    C_Terminal* pterminal = new C_Terminal(inW, inH);
    int i=0;
    while(g_apprun)
    {
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
        m_vo->set_frame(m_vo, frame, 0);
        pterminal->InputNv21((unsigned char*)vir[0]);
        //CALC_FPS("g_apprun");
    }
    delete pterminal;

    libmaix_camera_module_deinit();
    libmaix_image_module_deinit();
    std::cout << "main end!" << std::endl;
    return 0;

}

#if 0
int main(int argc, char **argv)
{
    
    std::cout << "main start!" << std::endl;
    libmaix_camera_module_init();
    std::cout << "libmaix_camera_module_init();" << std::endl;
    libmaix_image_module_init();
    std::cout << "libmaix_image_module_init();" << std::endl;
    struct libmaix_disp *disp = libmaix_disp_create(0);
    struct libmaix_cam* camera = libmaix_cam_create(0, inW, inH, 1, 0);
    if(camera == nullptr){
        std::cout << "main camera == nullptr!" << std::endl;
        return 0;
    }

    std::cout << "libmaix_cam_create();" << std::endl;

    camera->start_capture(camera);


    //C_h264enc* myEnc = new C_h264enc(inW, inH, inW, inH);
    std::cout << "start_capture();" << std::endl;
    libmaix_image_t *tmp = nullptr;
    for(int i=0; i<200; i++){
        camera->capture_image(camera, &tmp);
        disp->draw_image(disp, tmp);
        //myEnc->InputRgb888((unsigned char*)tmp->data);
        //std::cout << "tmp->data:;" << std::hex << tmp->data << std::endl;
    }
    if (LIBMAIX_ERR_NONE == camera->capture_image(camera, &tmp))
    {
        printf("w %d h %d p %d \r\n", tmp->width, tmp->height, tmp->mode);
    }
    std::cout << "capture_image();" << std::endl;

    // 创建一个输出文件流对象
    std::ofstream outfileRGB("RGB.data", std::ios::binary);
    // 检查文件是否成功打开
    if (!outfileRGB.is_open()) {
        std::cerr << "无法打开文件 " << "RGB.data" << " 进行写入！" << std::endl;
        return 1;
    }
    outfileRGB.write((const char *)tmp->data, inW*inH*3);

        // 延时3秒
    std::this_thread::sleep_for(std::chrono::seconds(3));

    libmaix_camera_module_deinit();
    libmaix_image_module_deinit();
    std::cout << "main end!" << std::endl;
    return 0;
}
#endif