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
    static constexpr int kTmpBuffSize = 2048; //临时缓冲区大小
public:
    C_ClientConnect(int sockeFd, std::map<std::string, unsigned int>& fileMap, std::mutex& fileMapMutex);
    ~C_ClientConnect();
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

    bool          m_bRunFlag;                  //线程运行标识
    std::unique_ptr<std::thread> m_pThread;    //接收客户端连接线程

};