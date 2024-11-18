#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <iostream>
#include <chrono>
#include <ctime>
#include <sstream>
#include <iomanip> // 用于设置输出格式
#include <cmath>   // 用于计算时差
#include "logAdapt.h"

/*没有继承C_LogAdapt类的或是C函数里进行打印使用该全局类接口*/
C_LogAdapt  gs_objLogNormal;

/*网络自适应模块不同级别日志输出接口*/
void C_LogAdapt::LogInner(const char *pscLevel, const char *pscFile, const char *pscFunc, unsigned int uiLine, const char *pscFmt, ...)	
{
	int         siRetVal;                       
	char        ascFormat[kMaxLogLen] = {0};
                                   
	siRetVal = snprintf(ascFormat, kMaxLogLen, "%s %s:[ %s ]<%s:%d:%s>: %s",
		GetCurrentDateTimeInChina().c_str(),
	    pscLevel,
		m_ossKey.str().c_str(),
		getFileName((char *)pscFile), 
		uiLine, pscFunc, pscFmt);           
	if (siRetVal < 0)                                               
	{
		std::cout << "Log Snprintf Filed" << std::endl;
		return;                                                   
	}                                                              
	                                                          
	char ascLogBuf[kMaxLogLen + 1] = { 0 };
	va_list stLogAp;
	va_start(stLogAp, pscFmt);                                     
	vsnprintf(ascLogBuf, kMaxLogLen, ascFormat, stLogAp);
	va_end(stLogAp);

	std::cout << ascLogBuf << std::endl; //输出详细信息日志

	//在控制日志写入文件时将日志写入文件中
	if(true){ 
		//智能指针删除器
		auto fileDeleter = [](std::ofstream* pobj){ pobj->close(); delete pobj; };
		//使用静态智能指针，程序退出后资源释放文件正常关闭
		static auto outputFile = std::unique_ptr<std::ofstream, decltype(fileDeleter)>(
			new std::ofstream("run.log", std::ios::out | std::ios::binary),
			fileDeleter
		);
		//文件正常打开时进行写入
		if(outputFile->is_open()){
			outputFile->write(ascLogBuf, strlen(ascLogBuf));
			//立即刷新到磁盘避免程序异常退出日志丢失
			outputFile->flush(); 
		}
	}
}

char* C_LogAdapt::getFileName(char *pucFileWithPath)
{
	/*去除windows格式的文件前缀*/
	char *pscBaseName = strrchr(pucFileWithPath, '\\');
	if (pscBaseName == nullptr)
	{
		/*去除linux格式的文件前缀*/
		pscBaseName = strrchr(pucFileWithPath, '/');
	}

	return (nullptr == pscBaseName) ? pucFileWithPath : (pscBaseName + 1);
}


// 中国时区偏移量（UTC+8）
constexpr int CHINA_TIME_OFFSET = 8 * 60 * 60; // 秒

// 定义一个函数，返回包含当前中国时间的字符串
std::string C_LogAdapt::GetCurrentDateTimeInChina() {
    // 获取当前时间点
    auto now = std::chrono::system_clock::now();

    // 转换为time_t类型以便于获取本地时间
    std::time_t now_c = std::chrono::system_clock::to_time_t(now);

    // 获取UTC时间结构
    //std::tm utc_time = *std::gmtime(&now_c);

    // 将UTC时间转换为中国时间
    std::time_t china_time_t = now_c + CHINA_TIME_OFFSET;
    std::tm china_time = *std::localtime(&china_time_t);

    // 获取毫秒部分
    auto duration = now.time_since_epoch();
    auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count() % 1000;

    // 使用字符串流来格式化时间
    std::ostringstream oss;
    oss << (china_time.tm_year + 1900) << "-" // 年份需要加1900
        << std::setw(2) << std::setfill('0') << (china_time.tm_mon + 1) << "-" // 月份需要加1，并填充到2位
        << std::setw(2) << std::setfill('0') << china_time.tm_mday << " "
        << std::setw(2) << std::setfill('0') << china_time.tm_hour << ":"
        << std::setw(2) << std::setfill('0') << china_time.tm_min << ":"
        << std::setw(2) << std::setfill('0') << china_time.tm_sec << "."
        << std::setw(3) << std::setfill('0') << millis; // 毫秒部分填充到3位

    // 返回格式化后的字符串
    return oss.str();
}
