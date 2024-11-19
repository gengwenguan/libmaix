/********************************************************************************* 
  *Copyright(C),Your Company 
  *FileName:  filemng.h
  *Author:    gengwenguan
  *Date:      2024-11-19
  *Description:  文件管理类，保存程序运行所产生的文件，同时限制文件数量，自带删除最老的文件
**********************************************************************************/ 
#pragma once
#include<string>
#include <fstream>
class C_FileMng
{
private:
    static constexpr const char* kFileDir = "video/";       //存放保存视频文件的路径
    static constexpr int kMaxFileNum = 10;                  //最多保存的文件数量
    static constexpr unsigned int kMaxFileSize =  1024 * 1024 * 1024; //每个文件最大容量1G
public:
    C_FileMng(/* args */);
    ~C_FileMng();

    //送入文件数据,内部进行保存并进行多文件管理
    void InputFileData(unsigned char* data, unsigned int dataLen);

private:
    //根据当前时间生成文件名
    std::string GenerateFilePathByNowTime();
    //保证文件数量在限制最大数量以内
    void KeepLatestFiles();

private:
    std::ofstream m_outFile;
    unsigned int m_fileSize{0};
    /* data */
};


