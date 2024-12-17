#include<cstring>
#include "shellServer.h"
#include "logAdapt.h"


/** @fn     GetInstance
*   @brief  获取单例对象
*   @param
*   @return
*/
C_ShellServer& C_ShellServer::GetInstance()
{
	static C_ShellServer instance;
	return instance;
}

C_ShellServer::C_ShellServer()
	:m_scRecvBuf(new char[RECV_BUF_LEN]),
	m_scSendBuf(new char[SEND_BUF_LEN]),
	m_bShellNetTaskRun(true),
	m_pthreadShellNetTask(new std::thread([this]() {this->ShellServerNetTask(); }))
{

}

C_ShellServer::~C_ShellServer(void)
{
	m_bShellNetTaskRun = false;       /*配置线程不再继续运行*/
	m_pthreadShellNetTask->join();    /*等待线程退出*/

	std::lock_guard<std::mutex> lock(m_shellUnitsMutex);
	m_ShellUnits.clear();   //取消所有的注册命令
}


/*************************************************
* Function:       RegShellCmd()
* Description:    注册shell命令
*                   (每次注册一个shell命令，就动态分配一块内存连接在shell管理列表中)
* Access Level:   public
* Input:          shellCmdName---shell命令名
*                 callbackFunc---shell命令回调函数
* Output:         N/A
* Return:         0 OK  /  -1 ERROR
*************************************************/
int C_ShellServer::RegShellCmd(const char* shellCmdName, std::function<int(void *, const char *, char *)> callbackFunc, void* callbackParam)
{
	if (shellCmdName == nullptr || callbackFunc == nullptr || strlen(shellCmdName) + 1 > MAX_SHELLCMD_LEN)
	{
		return -1;
	}

	std::lock_guard<std::mutex> lock(m_shellUnitsMutex);

	//先检查是否已经注册过该命令
	for (auto& pUnit : m_ShellUnits)  
	{
		if (strcmp(shellCmdName, pUnit->cmdName) == 0)
		{
			//CLOG_ERR("shell cmd:%s had exist!\n", shellCmdName);
			return -1;
		}
	}

	std::unique_ptr<SHELL_SERVER_UNIT> pShellUnit(new SHELL_SERVER_UNIT());
	memcpy(pShellUnit->cmdName, shellCmdName, strlen(shellCmdName) + 1);
	pShellUnit->callbackFcn = callbackFunc;
	pShellUnit->callbackParam = callbackParam;
	//将新注册的命令放入注册命令集合中
	m_ShellUnits.push_back(std::move(pShellUnit));
	//CLOG_INF("axxx RegShellCmd Cmd<%s>", shellCmdName);
	return 0;
}

/* 取消注册 */
int C_ShellServer::UnRegShellCmd(const char* shellCmdName)
{
	CLOG_INF("axxx UnRegShellCmd Cmd<%s>", shellCmdName);
	std::lock_guard<std::mutex> lock(m_shellUnitsMutex);
	auto iter = m_ShellUnits.begin();
	while (iter != m_ShellUnits.end())
	{
		if (strcmp(shellCmdName, (*iter)->cmdName) == 0){
			iter = m_ShellUnits.erase(iter);
		}
		else{
			++iter;
		}
	}
	return 0;
}

/*对客户端发来的命令进行分析处理，处理结果为在retInfo中*/
int C_ShellServer::analysisShellCmd(unsigned int argc, char *cmdName, char* cmdPara, char* retInfo)
{
    CLOG_ERR("Cmd:%s analysisShellCmd enter!", cmdName);
	if (cmdName == nullptr || (argc == 2 && cmdPara == nullptr) )
	{
        CLOG_ERR("axxx analysisShellCmd argc error!");
		return -1;
	}

	/*找到命令对应的shell单元*/
	std::lock_guard<std::mutex> lock(m_shellUnitsMutex);
	//如果是帮助字符串命令则返回所有支持的命令
	if (memcmp(cmdName, SHELL_SERVER_HELP, strlen(SHELL_SERVER_HELP)) == 0) {
		sprintf(retInfo + strlen(retInfo), "%-25s\n", SHELL_SERVER_HELP);
		for (auto& pUnit : m_ShellUnits)
		{
			sprintf(retInfo + strlen(retInfo), "%-25s\n", pUnit->cmdName);
		}
		CLOG_INF("Cmd:%s analysisShellCmd find!", SHELL_SERVER_HELP);
		return 0;
	}
	//找到对应的shell命令执行相应的回调函数返回相关字符串数据
	for (auto& pUnit : m_ShellUnits)
	{
		//这里只能用memcmp不能用strcmp，因为cmdName后面没有字符串结尾，后续的字符串结尾在cmdPara后面
		if (memcmp(cmdName, pUnit->cmdName, strlen(pUnit->cmdName)) == 0)
		{
			CLOG_INF("Cmd:%s analysisShellCmd find!", pUnit->cmdName);
			pUnit->callbackFcn(pUnit->callbackParam, cmdPara, retInfo);
			return 0;
		}
	}

	CLOG_ERR("Cmd:%s analysisShellCmd not find!", cmdName);
	return -1;
}

/*校验接收的数据头*/
int C_ShellServer::checkHeader(CMD_HEADER* pHeader)
{
	if (pHeader->length > sizeof(CMD_HEADER) + RECV_BUF_LEN
		|| pHeader->cmdLen > MAX_SHELLCMD_LEN
		|| pHeader->argLen > RECV_BUF_LEN - MAX_SHELLCMD_LEN)
	{
		return -1;
	}
	return 0;
}


#if (defined(_WIN32) || defined(_WIN32_WCE) || defined(WIN64))
#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>
#pragma comment(lib, "ws2_32.lib")

/*使用socket进行通信的shell服务器线程*/
void C_ShellServer::ShellServerNetTask(void)
{
	WSADATA wsa_data;
	int result;
	SOCKET server_socket, client_socket;
	struct sockaddr_in server_addr, client_addr;
	int client_addr_len;

	CMD_HEADER header;
	int leftlen;

	// 初始化 WinSock
	result = WSAStartup(MAKEWORD(2, 2), &wsa_data);
	if (result != 0) 
	{
		CLOG_ERR("WSAStartup failed with error: %d", result);
		return ;
	}

	// 创建套接字
	server_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

	// 绑定套接字
	server_addr.sin_family = AF_INET;
	server_addr.sin_addr.s_addr = INADDR_ANY; //如果只允许本机设备连接，此处设置为127.0.0.1 - 0x7f000001
	server_addr.sin_port = htons(LISTEN_PORT);  // 服务器监听端口为19123，客户端连接时连接该端口
	result = bind(server_socket, reinterpret_cast<sockaddr*>(&server_addr), sizeof server_addr);
	if (result == SOCKET_ERROR) 
	{
		CLOG_ERR("bind failed with error: %d", WSAGetLastError());
		closesocket(server_socket);
		WSACleanup();
		return ;
	}

	// 监听套接字
	result = listen(server_socket, QLEN);
	if (result == SOCKET_ERROR) 
	{
		CLOG_ERR("listen failed with error: %d", WSAGetLastError());
		closesocket(server_socket);
		WSACleanup();
		return ;
	}

	while (m_bShellNetTaskRun)
	{
		// 接受客户端连接
		client_addr_len = sizeof client_addr;
		client_socket = accept(server_socket, reinterpret_cast<sockaddr*>(&client_addr), &client_addr_len);
		if (client_socket == INVALID_SOCKET) 
		{
			CLOG_ERR("accept failed with error: %d", WSAGetLastError());
			std::this_thread::sleep_for(std::chrono::milliseconds(100)); /*sleep 100ms*/
			continue;
		}

		do {
			result = recv(client_socket, (char*)&header, sizeof(CMD_HEADER), 0);
			if (result == SOCKET_ERROR)
			{
				CLOG_ERR("read header failed with error: %d", WSAGetLastError());
				break;
			}

			/*校验接收的数据头*/
			if (checkHeader(&header) == -1)
			{
				CLOG_ERR("header error: %d %d %d", header.length, header.cmdLen, header.argLen);
				break;
			}

			memset(m_scRecvBuf.get(), 0, RECV_BUF_LEN);
			memset(m_scSendBuf.get(), 0, SEND_BUF_LEN);
			/*接收shell命令和参数*/
			if ((leftlen = header.length - sizeof(CMD_HEADER)) > 0)
			{
				if (leftlen >= RECV_BUF_LEN)
				{
					CLOG_ERR("data is too large: %d", leftlen);
					break;
				}

				result = recv(client_socket, m_scRecvBuf.get(), leftlen, 0);
				if (result == SOCKET_ERROR)
				{
					CLOG_ERR("read header failed with error: %d", WSAGetLastError());
					break;
				}
				// 字符串最后加上结束符
				m_scRecvBuf[leftlen] = '\0';
			}

			/*解析shell命令*/
			if (analysisShellCmd(header.argc, m_scRecvBuf.get(), m_scRecvBuf.get() + header.cmdLen, m_scSendBuf.get()) == -1)
			{
				strcat(m_scSendBuf.get(),  "analysis shell cmd failed!\n");
			}

			/*发送shell命令响应*/
			if (strlen(m_scSendBuf.get()) > 0)
			{
				result = send(client_socket, m_scSendBuf.get(), strlen(m_scSendBuf.get()), 0);
				if (result == SOCKET_ERROR)
				{
					CLOG_ERR("write failed with error: %d", WSAGetLastError());
				}
			}

		} while (0);

		// 关闭套接字
		closesocket(client_socket);
	}

	// 关闭监听套接字
	closesocket(server_socket);

	// 清理 Winsock 库
	WSACleanup();

	CLOG_INF("shellServerNetTask  server exit!\n");
}


#else

#include <sys/un.h>
#include <stddef.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/stat.h>

/*使用socket进行通信的shell服务器线程*/
void C_ShellServer::ShellServerNetTask(void)
{

	//// 获取当前线程的 ID  
	//auto threadId = pthread_self();
	//// 设置当前线程名称
	//pthread_setname_np(threadId, "shellServerNetTask");


	int server_sockfd, client_sockfd;
	int server_len, client_len;
	struct sockaddr_in server_address, client_address;
	CMD_HEADER header;
	int leftlen;

	// 创建套接字
	server_sockfd = socket(AF_INET, SOCK_STREAM, 0);

	// 命名套接字
	server_address.sin_family = AF_INET;
	server_address.sin_addr.s_addr = htonl(INADDR_ANY); //如果只允许本机设备连接，此处设置为127.0.0.1 - 0x7f000001
	server_address.sin_port = htons(LISTEN_PORT);
	server_len = sizeof(server_address);
	bind(server_sockfd, (struct sockaddr *) &server_address, server_len);

	// 监听套接字
	listen(server_sockfd, QLEN);

	while (m_bShellNetTaskRun)
	{
		CLOG_INF( "axxx  init");
		// 接受客户端连接
		client_len = sizeof(client_address);
		client_sockfd = accept(server_sockfd, (struct sockaddr *) &client_address, (socklen_t *)&client_len);
		CLOG_INF( "axxx  client_sockfd");
		/*接收shell交互命令头*/
		if ( read(client_sockfd, &header, sizeof(CMD_HEADER)) < 0)
		{
			CLOG_ERR("read header fail, errno is %d(%s)", errno, strerror(errno));
			close(client_sockfd);
			continue;
		}

		/*校验接收的数据头*/
		if (checkHeader(&header) == -1)
		{
			CLOG_ERR("header error: %d %d %d", header.length, header.cmdLen, header.argLen);
			close(client_sockfd);
			continue;
		}

		memset(m_scRecvBuf.get(), 0, RECV_BUF_LEN);
		memset(m_scSendBuf.get(), 0, SEND_BUF_LEN);
		/*接收shell命令和参数*/
		if ((leftlen = header.length - sizeof(CMD_HEADER)) > 0)
		{
			if ((unsigned int)leftlen >= RECV_BUF_LEN)
			{
				CLOG_ERR("data is too large: %d", leftlen);
				close(client_sockfd);
				continue;
			}

			if (read(client_sockfd, m_scRecvBuf.get(), leftlen) < 0)
			{
				CLOG_ERR("read left data fail, errno is %d(%s)", errno, strerror(errno));
				close(client_sockfd);
				continue;
			}

			m_scRecvBuf[leftlen] = '\0';
		}

		/*解析shell命令*/
		CLOG_INF("client shell cmd is %s", m_scRecvBuf.get());

		if (analysisShellCmd(header.argc, m_scRecvBuf.get(), m_scRecvBuf.get() + header.cmdLen, m_scSendBuf.get()) == -1)
		{
			strcat(m_scSendBuf.get(), "analysis shell cmd failed!\n");
		}

		/*发送shell命令响应*/
		if (strlen(m_scSendBuf.get()) > 0)
		{
			if (write(client_sockfd, m_scSendBuf.get(), strlen(m_scSendBuf.get())) < 0)
			{
				CLOG_ERR("write fail, errno is %d(%s)", errno, strerror(errno));
			}
		}

		// 关闭套接字
		close(client_sockfd);
	}

	return;
}

#endif


