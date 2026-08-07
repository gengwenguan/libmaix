#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <atomic>
#include <iostream>
#include <thread>
#include <chrono>
#include <ctime>
#include <iomanip> // 用于设置输出格式
#include <cmath>   // 用于计算时差
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif
#include "logAdapt.h"

/*没有继承C_LogAdapt类的或是C函数里进行打印使用该全局类接口*/
C_LogAdapt  gs_objLogNormal;

// 全局日志 sink：读多写极少（仅启动注册 / 退出注销），用原子指针发布即可，
// 热路径读侧无锁。std::memory_order_acquire/release 保证 sink 对象对读线程可见。
static std::atomic<ILogSink*> gs_pLogSink{nullptr};

// 当前线程是否抑制向 sink 分叉（见 logAdapt.h 说明）。thread_local 保证推送线程
// 与递归调用互不影响别的业务线程。
static thread_local bool gs_bThreadLogSuppressed = false;

void SetLogSink(ILogSink* sink)
{
    gs_pLogSink.store(sink, std::memory_order_release);
}

void SetThreadLogSuppressed(bool suppressed)
{
    gs_bThreadLogSuppressed = suppressed;
}

/*网络自适应模块不同级别日志输出接口*/
void C_LogAdapt::LogInner(const char *pscLevel, const char *pscFile, const char *pscFunc, unsigned int uiLine, const char *pscFmt, ...)	
{
	int         siRetVal;
	char        ascFormat[kMaxLogLen] = {0};

	siRetVal = snprintf(ascFormat, kMaxLogLen, "%s thread_id:%d %s:[ %s ]<%s:%d:%s>: %s",
		GetCurrentDateTimeInChina().c_str(),
		get_os_thread_id(),
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

	std::cout << ascLogBuf; //输出详细信息日志

	//在控制日志写入文件时将日志写入文件中
	if(true){ 
		//智能指针删除器
		auto fileDeleter = [](std::ofstream* pobj){ pobj->close(); delete pobj; };
		//使用静态智能指针，程序退出后资源释放文件正常关闭
		static auto outputFile = std::unique_ptr<std::ofstream, decltype(fileDeleter)>(
			new std::ofstream("run.log", std::ios::out | std::ios::binary),
			fileDeleter
		);
		static unsigned int fileSize = 0; //统计写入的文件大小

		//文件正常打开时进行写入
		if(outputFile->is_open()){
			outputFile->write(ascLogBuf, strlen(ascLogBuf));
			//立即刷新到磁盘避免程序异常退出日志丢失
			outputFile->flush(); 

			//日志大小达到10M时进行关闭重新创建，避免出现日志所占存储无限增长的情况
			fileSize += strlen(ascLogBuf);
			if(fileSize > 10 * 1024 * 1024){
				fileSize = 0;
				outputFile->close();
				outputFile->open("run.log", std::ios::out | std::ios::binary);
			}

		}
	}

	// 分叉给 sink（如推送到 web）。无 sink 时仅一次原子 load，空载零开销。
	// 抑制标志避免"sink 内部/推送线程打的日志"再次回灌，杜绝自激与死锁。
	if (!gs_bThreadLogSuppressed) {
		ILogSink* sink = gs_pLogSink.load(std::memory_order_acquire);
		if (sink) {
			gs_bThreadLogSuppressed = true;   // 本次分叉期间若递归打日志则跳过 sink
			sink->OnLogLine(ascLogBuf, (unsigned int)strlen(ascLogBuf));
			gs_bThreadLogSuppressed = false;
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

//获取线程id，兼容windows，mac，linux平台
int C_LogAdapt::get_os_thread_id()
{
#ifdef _WIN32
	// 在 Windows 上使用 GetCurrentThreadId
	DWORD tid = GetCurrentThreadId();
	return static_cast<int>(tid);
#elif defined(__APPLE__)
	uint64_t tid;
	// 在 macOS 和 iOS 上使用 pthread_threadid_np
	pthread_threadid_np(pthread_self(), &tid);
	return static_cast<int>(tid);
#else
	// 在 Linux 上使用 syscall(SYS_gettid)
	pid_t tid = syscall(SYS_gettid);
	return static_cast<int>(tid);
#endif

}

// 中国时区偏移量（UTC+8）
constexpr int CHINA_TIME_OFFSET = 8 * 60 * 60; // 秒

// 定义一个函数，返回包含当前中国时间的字符串
std::string C_LogAdapt::GetCurrentDateTimeInChina(bool bNoMs) {
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
        << std::setw(2) << std::setfill('0') << china_time.tm_sec;
	if(!bNoMs)
		oss << "." << std::setw(3) << std::setfill('0') << millis; // 毫秒部分填充到3位

    // 返回格式化后的字符串
    return oss.str();
}
