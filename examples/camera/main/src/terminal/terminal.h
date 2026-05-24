/********************************************************************************* 
  *Copyright(C),Your Company 
  *FileName:  terminal.h
  *Author:    gengwenguan
  *Date:      2024-10-25
  *Description:  编码传输终端：YUV→H264 + ALSA→AAC，
                 经 fMP4 muxer 封装后由 LiveHub 广播给 WebSocket 客户端
**********************************************************************************/ 
#pragma once
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <vector>
#include <string>
#include "libmaix_cam.h"
#include "libmaix_disp.h"
#include "h264Enc.h"
#include "aacEnc.h"
#include "websocketServer.h"
#include "httpServer.h"
#include "fmp4Muxer.h"
#include "recorder.h"
#include "recordCleaner.h"
#include "snapshot.h"
#include "personDetector.h"
#include "motionDetector.h"
#include "tlsContext.h"
#include "talkPlayer.h"

class C_Terminal : public C_H264Enc::C_Listener,
                   public C_AacEnc::C_Listener,
                   public C_WebSocketServer::C_Listener,
                   public C_Fmp4Muxer::C_Listener
{
public:
    // aiCam：可选；若非 nullptr，本终端会把它注入 PersonDetector。
    // 必须由 main 在 cam0 之后立即 create + start_capture，再传进来（V831 ISP 顺序约束）。
    // 本类不 own，不 destroy。
    C_Terminal(unsigned int Wight, unsigned int Hight, libmaix_cam_t* aiCam = nullptr);
    ~C_Terminal();

    //送入采集数据
    int InputRgb888(unsigned char* inputData);

    //送入采集数据
    int InputNv21(unsigned char* inputData);

    // 取最新一帧 AI 检测结果（归一化中心点+宽高）。供 main 在 cam0 上叠加绘制。
    // detector 已停止 / 未推理 / 帧过期时返回空 vector。
    std::vector<C_PersonDetector::Box> GetLatestAiBoxes(int maxAgeMs = 1000) const;

    //获取设备的ipv4地址
    static std::string get_ipv4_address();

private:
    //rgb888转Nv21格式
    void rgb888ToNv21(const unsigned char* rgb, unsigned char* nv21, int width, int height);
private:
    //编码器回调的H264数据
    int OnOutputH264(unsigned char* data, unsigned int dataLen,
                     int64_t ptsUs, bool isKey) override;

    //音频编码回调的aac数据
    int OnOutputAac(unsigned char* data, unsigned int dataLen, int64_t ptsUs) override;

    /*WebSocket新客户端连接事件*/
    int OnNewWSClientConnect(int fd) override;

    /*WebSocket客户端断开连接事件*/
    int OnWSClientDisconnect(int fd) override;

    /*接收到WebSocket客户端消息*/
    int OnWSClientMessage(int fd, const std::vector<unsigned char>& data) override;

    /*握手完成回调（用于区分 /ws/talk vs /ws/live）*/
    void OnWSClientHandshake(int fd, const std::string& urlPath) override;

    /*fmp4Muxer 输出 init segment（ftyp+moov）*/
    void OnInitSegment(const uint8_t* data, size_t len) override;
    /*fmp4Muxer 输出一个 fragment（moof+mdat）*/
    void OnFragment   (const uint8_t* data, size_t len) override;

    // 拿到 SPS/PPS（H264）+ ASC（AAC）后惰性创建 muxer
    void TryInitMuxer();

private:
    unsigned int m_Wight;
    unsigned int m_Hight;

    std::unique_ptr<unsigned char[]> m_pNv12Buff;

    std::unique_ptr<C_WebSocketServer> m_pWsServer;      // WebSocket直播服务器（订阅 LiveHub）
    std::unique_ptr<C_WebSocketServer> m_pWsFileServer;  // WebSocket回放服务器
    std::unique_ptr<C_HttpServer>      m_pHttpServer;    // HTTP服务器
    std::unique_ptr<C_H264Enc>         m_pH264Enc;
    std::unique_ptr<C_AacEnc>          m_pAacEnc;

    // fMP4 muxer：在拿到 SPS/PPS（首帧 IDR 后）+ ASC 后才能创建
    std::unique_ptr<C_Fmp4Muxer>       m_pMuxer;

    // 录像（订阅 LiveHub，10 分钟一片滚动落盘）
    std::unique_ptr<C_RollingRecorder> m_pRecorder;
    std::unique_ptr<C_RecordCleaner>   m_pCleaner;
    std::string                        m_recordDir;   // 绝对路径，基于 exe 目录

    // 拍照（独立目录，不自动清理）
    std::unique_ptr<C_Snapshot>        m_pSnapshot;
    std::string                        m_snapshotDir;

    // 人形识别（自动拍照触发；模型路径与配置由 AppConfig 控制）
    std::unique_ptr<C_PersonDetector>  m_pPersonDetector;

    // 移动侦测（VMD：与 AI 检测平行的轻量级触发器，纯 CPU 帧差）
    std::unique_ptr<C_MotionDetector>  m_pVmd;

    // TLS 上下文（HTTPS / wss 共用），首次启动时基于 <exeDir>/cert/ 加载证书
    std::unique_ptr<C_TlsContext>      m_pTls;

    // "讲话" 单向语音（浏览器→开发板 OPUS 播放）
    std::unique_ptr<C_TalkPlayer>      m_pTalkPlayer;
    // 握手时识别出来的 talk 客户端 fd 集合，仅这些 fd 的 binary 帧走 OPUS 播放
    std::mutex                         m_talkFdsMutex;
    std::set<int>                      m_talkFds;

    // 注册 HTTP API（录像目录浏览/下载等）
    void RegisterHttpApis();
};
