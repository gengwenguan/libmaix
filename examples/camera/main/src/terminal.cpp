#include "terminal.h"
#include<iostream>


C_Terminal::C_Terminal(unsigned int Wight, unsigned int Hight)
    :m_Wight(Wight),
    m_Hight(Hight),
    m_pH264Enc(new C_h264enc(this, Wight, Hight, Wight, Hight)),
    m_pTcpServer(new C_TcpServer(this)),
    outputFile("ter.264", std::ios::out | std::ios::binary),
    m_pNv12Buff(new unsigned char[Wight*Hight+Wight*Hight/2])
{

}


C_Terminal::~C_Terminal(){
    outputFile.close();
}

//送人采集数据
int C_Terminal::InputRgb888(unsigned char* inputData)
{
    rgb888ToNv21(inputData, m_pNv12Buff.get(), m_Wight, m_Hight);

    return InputNv21(m_pNv12Buff.get());
}

//送人采集数据
int C_Terminal::InputNv21(unsigned char* inputData)
{
    return m_pH264Enc->InputData(inputData);
}


//编码器回调的H264数据
int C_Terminal::OnOutputH264(unsigned char* data, unsigned int dataLen)
{
    //std::cout << "dataLen:" << dataLen << std::endl;
    m_pTcpServer->SendH264(data, dataLen);
    //此处放开可进行h264文件写入
    //outputFile.write((const char*)data, dataLen);
}

/*新客户端连接事件*/
int C_Terminal::OnNewClientConnect(int fd)
{
    std::cout << "OnNewClientConnect socketfd:" << fd << std::endl;
    //新客户端加入连接时请求编I帧
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