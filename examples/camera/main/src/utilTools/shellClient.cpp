//
// shellClient.cpp: 用于网络连接shellServer获取server上的信息。
//

#ifdef WIN32
#define _CRT_SECURE_NO_WARNINGS 1
#include <stdio.h>
#include <stdlib.h>
#define _WINSOCK_DEPRECATED_NO_WARNINGS 1
#include<Winsock2.h>
#include <ws2tcpip.h>
#include <synchapi.h>
#pragma comment(lib,"ws2_32.lib")
#include "stdafx.h"
#else
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h> 
#include <sys/socket.h>
#include <netinet/in.h>
#endif
#include <errno.h>
#include <string.h>
#include <iostream>
#include <ctime>
//#include <libgen.h>


static SOCKET socket_client;         //本地创建的客户端socket
static struct sockaddr_in server_in; //用于存储服务器的基本信息

static int connect_shell_server(const char *ipaddr, const unsigned short port)
{
#ifdef WIN32
	//初始化WSA
	WORD sockVersion = MAKEWORD(2, 2);
	WSADATA wsaData;
	if (WSAStartup(sockVersion, &wsaData) != 0) {
		printf("WSAStartup error!");
		return -1;
	}
#endif

	socket_client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (INVALID_SOCKET == socket_client) {
		printf("invalid socket !");
		return -1;
	}

	// 设置超时时间为2秒
	int timeout = 2000; // 2 seconds in milliseconds
	setsockopt(socket_client, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
	setsockopt(socket_client, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));

	server_in.sin_family = AF_INET;    //IPV4协议族
	server_in.sin_port = htons(port);  //服务器的端口号
	server_in.sin_addr.s_addr = inet_addr(ipaddr); //服务IP
	if (connect(socket_client, (struct sockaddr *)&server_in, sizeof(server_in)) == SOCKET_ERROR)
	{
		printf("connect error=%d!\n", errno);//这个可以打印出错误码和原因
		return -1;
	}

	return socket_client;
}

static int disconnect_shell_server(int socketfd)
{
	if (socketfd >= 0) {
#ifdef  WIN32
		closesocket(socketfd);
#else
		close(socketfd);
#endif
	}
	return 0;
}

static int send_cmd_line(int socketfd, const char *basename, const char *params)
{
	char cmd_buff[1024] = { 0, };

	struct CMD_HEADER
	{
		int length;         /* shell交互总长度: 16+cmdLen+argLen */
		int argc;           /* shell参数个数:简单起见，限定最多为2! */
		int cmdLen;         /* argv[0]---cmd的长度 */
		int argLen;         /* argv[1]---cmd参数的长度 */
	}header;
	memset(&header, 0, sizeof(header));

	if (basename == NULL && params == NULL)
	{
		printf("invalid format, basename<%s>, params<%s>\n", basename, params);//这个可以打印出错误码和原因
		return -1;
	}


	int argc = params == NULL ? 1 : 2;
	if (argc == 2)
	{
		header.argLen = strlen(params) + 1;
	}

	header.cmdLen = strlen(basename) + 1;
	header.argc = argc;
	header.length = sizeof(header) + header.cmdLen + header.argLen;
	printf("argc=%d, cmd=<%s>, param=<%s>, len=[%d, %d, %d, %d]\n",
		argc, basename, params, header.length, (int)sizeof(header), header.cmdLen, header.argLen);//这个可以打印出错误码和原因

	int len = sizeof(header);
	memcpy(cmd_buff, (char *)&header, len);
	memcpy(cmd_buff + len, (char *)basename, header.cmdLen);
	if (argc == 2)
	{
		memcpy(cmd_buff + len + header.cmdLen, params, header.argLen);
	}

	send(socketfd, cmd_buff, header.length, 0);

	return 0;
}

static int receive_cmd_result(int socketfd, char *recv_buf, int buf_len)
{
	if (recv(socketfd, recv_buf, buf_len, 0) > 0)
	{
		printf("%s\n", recv_buf);
	}
	return 0;
}


int main(int argc, char* argv[])
{
	char ipaddr[128];
	unsigned int port = 0;;

	const char *config = "shell.cfg";
	char cmd_line[512] = { 0, };
	const unsigned int max_recv_buf_len = 0xa00000;
	char *recv_buf = (char*)malloc(max_recv_buf_len);

	const char *addrInfo = argv[1];
	sscanf(addrInfo, "%127[^:]:%d", ipaddr, &port);
	printf("get server(%s)<%s:%u> ok!  ", addrInfo, ipaddr, port);
	// 获取当前时间
	std::time_t now = std::time(nullptr);

	// 将时间转换为本地时间
	std::tm* localTime = std::localtime(&now);

	// 打印年月日时分秒
	std::cout << (localTime->tm_year + 1900) << '-'  // 年
		<< (localTime->tm_mon + 1) << '-'       // 月
		<< localTime->tm_mday << ' '            // 日
		<< localTime->tm_hour << ':'            // 时
		<< localTime->tm_min << ':'             // 分
		<< localTime->tm_sec << std::endl;      // 秒

#ifdef  WIN32
	const char *cmd_basename = argv[2];
#else
	const char *cmd_basename = basename(argv[1]);
#endif
	const char *cmd_params = argv[3];

	int socketfd = connect_shell_server(ipaddr, port);
	if (socketfd < 0) {
		printf("connect shell server<%s:%u> failed!\n", ipaddr, port);
		return -1;
	}

	if (send_cmd_line(socketfd, cmd_basename, cmd_params) < 0) {
		printf("send cmd<%s> params<%s> failed!\n", cmd_basename, cmd_params);
		disconnect_shell_server(socketfd);
		return -1;
	}

#ifdef  WIN32
	Sleep(100);
#else
	usleep(100 * 1000);
#endif

	receive_cmd_result(socketfd, recv_buf, max_recv_buf_len);

	disconnect_shell_server(socketfd);

	return 0;
}
