/********************************************************************************* 
  *Copyright(C),Your Company 
  *FileName:  logAdapt.h
  *Author:    gengwenguan
  *Date:      2024-10-25
  *Description:    日志适配类,使用该接口后可获得更为详细的日志输出,以及控制日志是否写入到文件中
**********************************************************************************/ 
#pragma once
#include <iostream>
#include <sstream>
#include <fstream>

/*C_LogAdapt类，
* 该类提供的打印接口提供以下日志输出格式
* 日志格式:日志产生时间 日志级别:[ 自定义key信息 ]<文件名:函数名:行号>: 日志
*/
class C_LogAdapt 
{
private：
	static constexpr int kMaxLogLen = 1024;             //最大支持日志输出长度
	static constexpr bool kWriteFile = true;            //是否将每条日志写入到文件
	static constexpr const char* kFileName = "run.log"; //开启日志写入文件时的日志文件名
public:
	/* 获取日志输出的key，进行设置后可用于区分同一个类多个对象的打印信息 */
	// GetLogKey() << "自定义信息"  ;
	// 按照以上方式设置，可输出带自定义信息的日志
	std::ostringstream& GetLogKey() { return m_ossKey; }

	/*不同级别日志输出接口*/
	void LogInner(const char *pscLevel, const char *pscFile, const char *pscFunc, unsigned int uiLine, const char *pscFmt, ...);

private:

	/*从文件存放路径中提取文件名，兼容windows和Linux平台*/
	char *getFileName(char *pucFileWithPath);
	//获取当前系统时间
	std::string getCurrentDateTimeInChina();

private:
	std::ostringstream m_ossKey;  /* 区分不同对象的key信息 */
};


/*下面的宏调用，在C++代码中使用，需要正在继承了C_LogAdapt的成员函数中，否则会编译报错*/
#define NLOG_FLT(...)      C_LogAdapt::LogInner("FLT", __FILE__,   __FUNCTION__,  __LINE__,   __VA_ARGS__)
#define NLOG_ERR(...)      C_LogAdapt::LogInner("ERR", __FILE__,   __FUNCTION__,  __LINE__,   __VA_ARGS__)
#define NLOG_WRN(...)      C_LogAdapt::LogInner("WRN", __FILE__,   __FUNCTION__,  __LINE__,   __VA_ARGS__)
#define NLOG_INF(...)      C_LogAdapt::LogInner("INF", __FILE__,   __FUNCTION__,  __LINE__,   __VA_ARGS__)


/*没有继承C_LogAdapt类的或是C函数里进行打印使用该全局类接口*/
extern C_LogAdapt   gs_objLogNormal;

/*下面的宏调用，在C代码中使用或者没有继承C_LogAdapt类的接口中使用*/
#define CLOG_FLT(...)   gs_objLogNormal.LogInner("FLT", __FILE__,   __FUNCTION__,  __LINE__,   __VA_ARGS__)
#define CLOG_ERR(...)   gs_objLogNormal.LogInner("ERR", __FILE__,   __FUNCTION__,  __LINE__,   __VA_ARGS__)
#define CLOG_WRN(...)   gs_objLogNormal.LogInner("WRN", __FILE__,   __FUNCTION__,  __LINE__,   __VA_ARGS__)
#define CLOG_INF(...)   gs_objLogNormal.LogInner("INF", __FILE__,   __FUNCTION__,  __LINE__,   __VA_ARGS__)
