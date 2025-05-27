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
#include <atomic>
#include <memory>
#include "clientConnect.h"
//#include <alsa/asoundlib.h>
class C_FileMng : public C_ClientConnect::C_Listener
{
private:
    static constexpr const char* kFileDir = "video/";       //存放保存视频文件的路径
    static constexpr int kMaxFileNum = 10;                  //最多保存的文件数量
    static constexpr unsigned int kMaxFileSize = 1024 * 5000; //1024 * 1024 * 1024; //每个文件最大容量1G
    static constexpr int kFileMngPort  = 56060;             //文件管理服务监听端口
    static constexpr int kMoovHeadLen  = 100 * sizeof(long long); //文件moov头长度，其中用来保存均匀的100个I帧位置
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

    //送入文件数据,内部进行保存并进行多文件管理, flag = 0-音频opus数据 1-视频h264数据
    void InputFileData(unsigned char* data, unsigned int dataLen, char flag);

private:
    //接收客户端连接
    int Accept();
    //根据当前时间生成文件名
    std::string GenerateFilePathByNowTime();
    //获取文件大小
    long long GetFileSize(std::string filePath);

    //将一个vector中的值均匀抽取成100个vector
    std::vector<long long> UniformResizeTo100(const std::vector<long long>& IdrPos);

    //获取文件map表锁
    virtual std::mutex& GetfileMapMutex() override { return m_fileMapMutex; }
    //获取文件map表
    virtual std::map<std::string, std::vector<long long>>& GetfileMap() override { return m_fileMap; }
    //获取最后一个文件大小
    virtual long long GetLastFileSize() override { return m_LastfileSize.load(); }
    //获取临时100I帧位置
    std::vector<long long> GetTmpIdrPos100() { return UniformResizeTo100(m_AllIdrPos); }


private:
    C_Listener*  m_pListrner;                   //监听器
    std::mutex    m_outFileMutex;     //互斥锁，h264和opus生产线程可能同时操作文件对象进行数据写入，此处加锁保护
    std::string   m_outfilePath;   //正在写入文件名
    std::ofstream m_outFile;       //正在写入文件对象
    std::atomic<long long>  m_LastfileSize{0};  //最新文件大小
    std::vector<long long>  m_AllIdrPos;        //I帧在文件中的偏移位置

    bool          m_bRunFlag;                  //线程运行标识
    std::thread   m_Thread;    //接收客户端连接线程

    int           m_server_fd;
    std::mutex    m_oMutex;    //互斥锁
    std::map<int, std::unique_ptr<C_ClientConnect>> m_fdConnections;     //客户端连接集合

    std::mutex    m_fileMapMutex;    //互斥锁
    std::map<std::string, std::vector<long long>> m_fileMap;
};


