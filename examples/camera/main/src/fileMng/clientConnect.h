/********************************************************************************* 
  *Copyright(C),Your Company 
  *FileName:  clientConnect.h
  *Author:    gengwenguan
  *Date:      2024-11-19
  *Description:  文件回放连接类，每个对象对应一个与回放客户端连接的socket
**********************************************************************************/ 
#pragma once
#include<string>
#include <fstream>
#include <iostream>
#include <thread>
#include <mutex>
#include <set>
#include <map>
#include <memory>
//#include <alsa/asoundlib.h>

class C_ClientConnect
{
private:
    static constexpr int kTmpBuffSize = 2048;    //临时缓冲区大小
    static constexpr int kIntervalMs = 30;       //视频正常传输时两帧间隔时间
    static constexpr int kJumpPercentage = 300;  //快进或者快退的比例，300的话每次前进或者后退整个文件的1/300
public:
    C_ClientConnect(int sockeFd, std::map<std::string, unsigned int>& fileMap, std::mutex& fileMapMutex);
    ~C_ClientConnect();

    //接收到取流端发来的控制消息：0~100进度条拖动、101快退, 102快进、103取消快进、104上一个文件、105下一个文件
    int RecvCtrlMesssage(char* pData, unsigned int nLen);
private:
    //向回放客户端发送存储的视频文件数据
    void SendFileData();
    //向客户端通过网络发送NAL数据
    int SendNal(char* pData, unsigned int nLen);
    //获取文件大小
    unsigned int GetFileSize(std::string filePath);

private:
    int m_sockeFd;
    std::mutex&    m_fileMapMutex;    //互斥锁
    std::map<std::string, unsigned int>&  m_fileMap;
    std::map<std::string, unsigned int>::iterator  m_fileMapIter;

    std::ifstream m_sendFile;
    unsigned int  m_sendFileSize;
    int           m_progress;                  //当前发送位置占总位置的百分比[0~100]

    bool          m_bRunFlag;                  //线程运行标识
    std::unique_ptr<std::thread> m_pThread;    //接收客户端连接线程

    bool          m_bNeedIframe;               //需要关键帧，被取流客户端快进或者快退时或者拖动进度条时保证第一帧为关键帧
};