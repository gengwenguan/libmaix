/**************************************************************************************
*  Copyright 2003-2010 Hangzhou Hikvision Digital Technology Co., Ltd.
*  Filename:              shellServer.h
*  Description:           shell服务器，内部启动TCP服务接收客户端连接，客户端发来相关命令后，服务器根据命令
*                         调用对应回调函数，回调函数执行输出的字符串再反馈给客户端
*  Author:                gengwenguan
*  Create:                2024-02-19
*  Modification history:
**************************************************************************************/
#pragma once

#include<vector>
#include<mutex>
#include<thread>
#include<functional>

class C_ShellServer
{
private:
	static constexpr unsigned short LISTEN_PORT        =     19123;          //ShellServer监听的tcp端口
	static constexpr unsigned int   QLEN               =     10;	         //最大监听客户端数量
	static constexpr unsigned int   RECV_BUF_LEN       =     1024;           //接收客户端发来的最大字符串长度
	static constexpr unsigned int   SEND_BUF_LEN       =     100 * 1024;     //服务器发给客户端支持的最大字符串长度
	static constexpr unsigned int   MAX_SHELLCMD_LEN   =     32;             //Shell命令支持的最大长度
	static constexpr const char *   SHELL_SERVER_HELP  =     "NetAdaptHelp"; //ShellServer帮助命令，端侧发来的该字符串返回所有支持的命令

	/*shell管理节点数据结构*/
	struct SHELL_SERVER_UNIT
	{
		char	         cmdName[MAX_SHELLCMD_LEN];	                                 /*shell命令名字*/
		std::function<int(void *, const char *recvStr, char *sendStr)> callbackFcn;  /*shell server管理单元回调函数*/
		void            *callbackParam;                                              /*回调函数参数*/
	};

public:
	/** @fn     GetInstance
	*   @brief  获取ShellServer单例对象
	*   @param
	*   @return
	*/
	static C_ShellServer& GetInstance();

	/*************************************************
	* Function:	      RegShellCmd()
	* Description:    注册shell命令
	*				     (每次注册一个shell命令，就动态分配一块内存连接在shell管理列表中)
	* Access Level:   public
	* Input:    	  shellCmdName---shell命令名
	*				  callbackFunc---shell命令回调函数
	*                 callbackParam--回调参数
	* Output:         N/A
	* Return:         0 成功
	*************************************************/
	int RegShellCmd(const char* shellCmdName, std::function<int(void *, const char *, char *)> callbackFunc, void* callbackParam);

	/* 取消注册 */
	int UnRegShellCmd(const char* shellCmdName);

private:
	/*使用socket进行通信的shell服务器线程*/
	void          ShellServerNetTask(void);

	/*对客户端发来的命令进行分析处理，处理结果为在retInfo中，将retInfo发送给客户端*/
	int           analysisShellCmd(unsigned int argc, char *cmdName, char* cmdPara, char* retInfo);

	/*shell交互命令头*/
	struct CMD_HEADER
	{
		unsigned int length;			/*shell交互总长度: 16+cmdLen+argLen*/
		unsigned int argc;			    /*shell参数个数:简单起见，限定最多为2!*/
		unsigned int cmdLen;			/*argv[0]---cmd的长度*/
		unsigned int argLen;			/*argv[1]---cmd参数的长度*/
	};
	/*校验接收的数据头*/
	int           checkHeader(CMD_HEADER* pHeader);

	C_ShellServer();
	~C_ShellServer();

private:
	std::unique_ptr<char[]>  m_scRecvBuf;           /*接收缓冲区*/
	std::unique_ptr<char[]>  m_scSendBuf;           /*发送缓冲区*/	

	bool                          m_bShellNetTaskRun;         /*网络shell线程运行状态*/
	std::unique_ptr<std::thread>  m_pthreadShellNetTask;      /*网络shell服务器线程句柄*/

	std::mutex          m_shellUnitsMutex;		                   /*单元数组锁*/
	std::vector<std::unique_ptr<SHELL_SERVER_UNIT>> m_ShellUnits;  /*存放管理数据单元指针的数组*/
};
