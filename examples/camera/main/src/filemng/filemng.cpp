#include <dirent.h>
#include <sys/stat.h>
#include <vector>
#include <algorithm>
#include"filemng.h"
#include"logAdapt.h"

C_FileMng::C_FileMng(/* args */)
{
    //要先确保存放视频的文件夹存在
    DIR *dir = opendir(kFileDir); // 打开目录
    if (dir == nullptr)
    {
        mkdir(kFileDir, 0775); // 打开失败时可能是由于目录不存在，尝试主动创建
        CLOG_ERR("Directory don`t exit create it now --- %s\n", kFileDir);
        dir = opendir(kFileDir); // 打开目录
        if (dir == nullptr)
        {
            CLOG_ERR("Failed to open %s\n", kFileDir);
            return;
        }
    }
    closedir(dir); // 关闭目录

    std::string fileName = GenerateFilePathByNowTime();
    CLOG_INF("fileName = %s!\n", fileName.c_str());
    //构造时先创建出来写入文件对象
    m_outFile.open(fileName.c_str(), std::ios::out | std::ios::binary);

    CLOG_INF("m_outFile.open =  %d!\n", m_outFile.is_open());
}

C_FileMng::~C_FileMng()
{
    if(m_outFile.is_open()){
        m_outFile.close();
    }
}

//送入文件数据
void C_FileMng::InputFileData(unsigned char* data, unsigned int dataLen)
{
    if(m_outFile.is_open()){
        m_outFile.write((const char*)data, dataLen);
        m_fileSize += dataLen;
        //文件达到最大内存限制时进行关闭，重新创建一个新文件
        if(m_fileSize >= kMaxFileSize){
            m_outFile.close();
            m_fileSize = 0;

            //生成新得到文件名
            std::string fileName = GenerateFilePathByNowTime();
            //新创建并打开一个文件
            m_outFile.open(fileName.c_str(), std::ios::out | std::ios::binary);
            //保证文件数量在最大限制之内
            KeepLatestFiles();
        }
    }else{
        CLOG_ERR("m_outFile not open!\n");
    }
}

//根据当前时间生成文件名
std::string C_FileMng::GenerateFilePathByNowTime()
{
    time_t timep;
    struct tm *p;
    char name[128] = {0};
    time(&timep);                                                                                                           // 获取从1970至今过了多少秒，存入time_t类型的timep
    p = localtime(&timep);
    p->tm_hour += 8;
    timep = mktime(p);
    p = localtime(&timep);
                    
    sprintf(name, "%d_%02d_%02d_%02d-%02d-%02d", 1900 + p->tm_year, 1 + p->tm_mon, p->tm_mday, p->tm_hour, p->tm_min, p->tm_sec); // 把格式化的时间写入字符数组中

    char fileAbsPath[256] = {0};
    sprintf(fileAbsPath, "%s%s%s", kFileDir, name, ".264");

    return std::string(fileAbsPath);
}

// 只保留目录下最新创建的n个文件，其余文件全部删除
void C_FileMng::KeepLatestFiles()
{
    DIR *dir = opendir(kFileDir); // 打开目录
    if (dir == nullptr)
    {
        mkdir(kFileDir, 0775); // 打开失败时可能是由于目录不存在，尝试主动创建
        CLOG_ERR("Directory don`t exit create it now --- %s\n", kFileDir);
        dir = opendir(kFileDir); // 打开目录
        if (dir == nullptr)
        {
            CLOG_ERR("Failed to open %s\n", kFileDir);
            return;
        }
    }

    struct dirent *entry = nullptr;
    std::vector<std::string> fileNames; // 定义文件列表，保存文件名和最后修改时间

    while ((entry = readdir(dir)) != nullptr) // 遍历目录下所有文件
    {
        if (entry->d_type == DT_REG) // 如果是普通文件
        {
            std::string filename = entry->d_name;
            fileNames.push_back(filename);
        }
    }

    closedir(dir); // 关闭目录

    if(fileNames.size() <= kMaxFileNum) return; //文件数量没达到上限时不需要删除

    std::sort(fileNames.begin(), fileNames.end());

    for (size_t i = 0; i < fileNames.size() - kMaxFileNum; i++) // 删除多余文件
    {
        std::string filepath = std::string(kFileDir) + fileNames[i];
        if (remove(filepath.c_str()) != 0)
        {
            CLOG_ERR("Failed to delete file: ", filepath.c_str());
        }
    }
}