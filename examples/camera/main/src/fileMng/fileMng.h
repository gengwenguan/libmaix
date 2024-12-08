/********************************************************************************* 
  *Copyright(C),Your Company 
  *FileName:  filemng.h
  *Author:    gengwenguan
  *Date:      2024-11-19
  *Description:  文件管理类，保存程序运行所产生的视频文件，同时限制文件数量，自动删除最老的文件
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
#include "clientConnect.h"
//#include <alsa/asoundlib.h>
class C_FileMng
{
private:
    static constexpr const char* kFileDir = "video/";       //存放保存视频文件的路径
    static constexpr int kMaxFileNum = 10;                  //最多保存的文件数量
    static constexpr unsigned int kMaxFileSize =  1024 * 1024 * 1024; //每个文件最大容量1G
    static constexpr int kFileMngPort  = 56060;             //文件管理服务监听端口
public:
    class C_Listener
    {
    public:
        virtual ~C_Listener() = default;
        /*新文件创建*/
        virtual int OnNewFileCreate() = 0;
    };
public:
    C_FileMng(C_Listener* pListener);
    ~C_FileMng();

    //送入文件数据,内部进行保存并进行多文件管理
    void InputFileData(unsigned char* data, unsigned int dataLen);

private:
    //接收客户端连接
    int Accept();
    //根据当前时间生成文件名
    std::string GenerateFilePathByNowTime();
    //获取文件大小
    unsigned int GetFileSize(std::string filePath);
private:
    C_Listener*  m_pListrner;                  //监听器
    std::ofstream m_outFile;
    unsigned int  m_fileSize{0};

    bool          m_bRunFlag;                  //线程运行标识
    std::unique_ptr<std::thread> m_pThread;    //接收客户端连接线程

    int           m_server_fd;
    std::mutex    m_oMutex;    //互斥锁
    std::map<int, std::unique_ptr<C_ClientConnect>> m_fdConnections;     //客户端连接集合

    std::mutex    m_fileMapMutex;    //互斥锁
    std::map<std::string, unsigned int> m_fileMap;
};


