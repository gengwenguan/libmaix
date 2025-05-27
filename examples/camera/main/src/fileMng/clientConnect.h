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
#include <atomic>
#include <list>
#include <vector>
#include"logAdapt.h"
//#include <alsa/asoundlib.h>

//C_ClientConnect会创建多个对象，继承C_LogAdapt设置不同的key对多个对象的打印进行区分
class C_ClientConnect : public C_LogAdapt
{
private:
    static constexpr int kTmpBuffSize = 4096;      //临时缓冲区大小
    static constexpr int kIntervalDefaultMs = 30;  //视频正常传输时两帧间隔时间
    static constexpr int kIntervalFastPlayMs = 7;  //视频加速播放时两帧间隔时间
    static constexpr int kJumpPercentage = 300;    //快进或者快退的比例，300的话每次前进或者后退整个文件的1/300
public:
    class C_Listener
    {
    public:
        virtual ~C_Listener() = default;
        //获取文件map表锁
        virtual std::mutex& GetfileMapMutex() = 0;
        //获取文件map表
        virtual std::map<std::string, std::vector<long long>>& GetfileMap() = 0;
        /*获取最新一个文件的大小*/
        virtual long long GetLastFileSize() = 0;

        //获取临时100I帧位置
        virtual std::vector<long long> GetTmpIdrPos100() = 0;
    };
public:
    C_ClientConnect(C_Listener* pListener, int sockeFd);
    ~C_ClientConnect();

    //接收到取流端发来的控制消息：0~100进度条拖动、101快退、102快进、104上一个文件、105下一个文件、107加速、108停止加速
    int RecvCtrlMesssage(char* pData, unsigned int nLen);
private:
    //处理接收到的控制消息
    int HandleCtrlMesssage();
    //向回放客户端发送存储的视频文件数据
    void SendFileTask();
    //向客户端通过网络发送Chuck数据
    int SendChuck(char* pData, unsigned int nLen);

    //检查是否到达文件结尾，如果到达则打开新的文件
    bool CheckSendFileEof();
    //最新生成的文件实时获取
    //void GetIdrPos100();

private:
    C_Listener*    m_pListener;
    int            m_sockeFd;

    //文件迭代器，指向文件管理类里面的文件map
    std::map<std::string, std::vector<long long>>::iterator  m_fileMapIter;

    std::ifstream m_sendFile;                  //当前正在发送的文件
    std::vector<long long>  m_IdrPos100 = std::vector<long long>(100); //I帧在文件中均匀的100个偏移位置
    char           m_progress;                  //当前发送位置占总位置的百分比[0~100]

    std::atomic<bool>            m_bRunFlag;   //线程运行标识
    std::thread                  m_Thread;     //接收客户端连接线程

    std::mutex               m_MessageMutex;    //消息列表锁
    std::list<unsigned char> m_MessageList;

    int           m_IntervalMs;                //两帧间隔时间
};