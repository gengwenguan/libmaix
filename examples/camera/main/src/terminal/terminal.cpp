#include<iostream>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>  // 包含这个头文件以确保 NI_MAXHOST 和 NI_NUMERICHOST 定义
#include"logAdapt.h"
#include "terminal.h"


C_Terminal::C_Terminal(unsigned int Wight, unsigned int Hight)
    :m_Wight(Wight),
    m_Hight(Hight),
    m_pTcpServer(new C_TcpServer(this)),
    m_pFileMng(new C_FileMng(this)),
    m_pH264Enc(new C_H264Enc(this, Wight, Hight, Wight, Hight)),
    m_pOpusEnc(new C_OpusEnc(this)),
    m_pNv12Buff(new unsigned char[Wight*Hight+Wight*Hight/2])
{

}


C_Terminal::~C_Terminal(){
}

//送入采集数据
int C_Terminal::InputRgb888(unsigned char* inputData)
{
    rgb888ToNv21(inputData, m_pNv12Buff.get(), m_Wight, m_Hight);

    return InputNv21(m_pNv12Buff.get());
}

//送入采集数据
int C_Terminal::InputNv21(unsigned char* inputData)
{
    return m_pH264Enc->InputData(inputData);
}


//编码器回调的H264数据
int C_Terminal::OnOutputH264(unsigned char* data, unsigned int dataLen)
{
    //打印编码出的帧信息
    //printf("%d %d %d %d %x %d ", data[0], data[1],data[2],data[3], data[4], data[4]&0x1f);
    //std::cout << "dataLen:" << dataLen << std::endl;

    //文件数据进行保存管理
    m_pFileMng->InputFileData(data, dataLen, 1);

    //此处可控制h264文件写入文件，用于临时测试数据是否正常
	if(false){ 
		//智能指针删除器
		auto fileDeleter = [](std::ofstream* pobj){ pobj->close(); delete pobj; };
		//使用静态智能指针，程序退出后资源释放文件正常关闭
		static auto outputFile = std::unique_ptr<std::ofstream, decltype(fileDeleter)>(
			new std::ofstream("encode.h264", std::ios::out | std::ios::binary),
			fileDeleter
		);
		//文件正常打开时进行写入
		if(outputFile->is_open()){
			outputFile->write((const char*)data, dataLen);
		}
	}

    //通过tcp将数据发送给客户端
    return m_pTcpServer->SendH264(data, dataLen);
}

//音频编码回调的opus数据
int C_Terminal::OnOutputOpus(unsigned char* data, unsigned int dataLen){
    //CLOG_INF("OnOutputOpus %d %d %d %d dataLen%d\n",data[0], data[1],data[2],data[3], dataLen);
    //文件数据进行保存管理
    m_pFileMng->InputFileData(data, dataLen, 0);

    //通过tcp将opus音频数据发送给正在连接预览画面的客户端
    return m_pTcpServer->SendOpus(data, dataLen);
}

/*新客户端连接事件*/
int C_Terminal::OnNewClientConnect(int fd)
{
    CLOG_INF("OnNewClientConnect socketfd:%d\n", fd);
    //新客户端加入连接时请求编I帧
    m_pH264Enc->ForceIframe();
    return 0;
}

/*新文件创建*/
int C_Terminal::OnNewFileCreate()
{
    CLOG_INF("OnNewFile Create!\n");
    //文件创建时请求编I帧。，保证文件能够正常打开播放
    m_pH264Enc->ForceIframe();
    return 0;
}

// RGB888 转 NV21
void C_Terminal::rgb888ToNv21(const unsigned char* rgb, unsigned char* nv21, int width, int height) {
    int yIndex = 0;
    int uvIndex = width * height;
    int index = 0;

    for (int j = 0; j < height; ++j) {
        for (int i = 0; i < width; ++i) {
            int r = rgb[index++];
            int g = rgb[index++];
            int b = rgb[index++];

            // 计算Y分量
            nv21[yIndex++] = static_cast<unsigned char>((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;

            // 计算U和V分量，每2x2的块共享一个U和一个V
            if (j % 2 == 0 && i % 2 == 0) {
                // 计算V和U分量（注意顺序，V在前，U在后）
                int v = static_cast<int>((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
                int u = static_cast<int>((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;

                // 存储V和U分量
                nv21[uvIndex++] = static_cast<unsigned char>(v);
                nv21[uvIndex++] = static_cast<unsigned char>(u);
            }
        }
    }
}

// 获取 IPv4 地址的接口
std::string C_Terminal::get_ipv4_address() {
    struct ifaddrs *ifaddr, *ifa;
    char host[NI_MAXHOST];
    std::string ipv4_address = "0.0.0.0";

    // 获取网络接口信息
    if (getifaddrs(&ifaddr) == -1) {
        perror("getifaddrs");
        return "";
    }

    // 遍历所有网络接口
    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL)
            continue;

        int family = ifa->ifa_addr->sa_family;

        // 只处理 IPv4 地址
        if (family == AF_INET) {
            int s = getnameinfo(ifa->ifa_addr, sizeof(struct sockaddr_in), host, NI_MAXHOST, NULL, 0, NI_NUMERICHOST);
            if (s != 0) {
                std::cerr << "getnameinfo() failed: " << gai_strerror(s) << std::endl;
                continue;
            }
            // 找到第一个 IPv4 地址并返回
            ipv4_address = host;
            if(ipv4_address == "127.0.0.1"){ continue; } //找到的为127.0.0.1本地回环地址跳过
            break;
        }
    }

    freeifaddrs(ifaddr); // 释放资源
    return ipv4_address;
}