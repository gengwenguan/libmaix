#if 0

#include <iostream>
#include <string>
#include <vector>
#include <fstream>
#include <memory>
#include <windows.h>
#include <mutex>
#include <queue>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/log.h>
#include <libavutil/samplefmt.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include "SDL3/SDL.h"
#include <SDL3/SDL_audio.h>
}

#include "rtpbase.h"
#include "RedParase.h"

std::ifstream inFile;
std::ofstream outFile;
AVCodecContext* codecCtx; //解码器上下文
SwrContext* swr;          //重采样句柄
AVFrame* frame;           
AVPacket *pkt;

//程序执行路径下找到一个.mp4结尾的文件名
std::string FindMp4File()
{
	WIN32_FIND_DATAA findData;
	HANDLE hFind = FindFirstFileA("*.mp4", &findData);
	std::string fileName;

	if (hFind != INVALID_HANDLE_VALUE) {
		do {
			if (!(findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
				fileName = findData.cFileName;
				break; // 找到第一个.mp4文件即停止
			}
		} while (FindNextFileA(hFind, &findData));
		FindClose(hFind);
	}
	return fileName;
}



// 音频回调函数，音频可播放时会触发回调，在该接口中将音频PCM数据送入
// 目前文件的读取以及opus的解码都在该回调接口中完成，后续可考虑在主线程中
// 读取文件进行解码，将解码数据放入队列中，该回调函数从队列中取数据进行播放比较合适
void AudioCallback(void* userdata, SDL_AudioStream* stream, int additional, int total)
{
	//std::cout << "additional:" << additional << "total" << total << std::endl;
	if (additional == 0) return; //需要播放数据长度为0时直接返回
	if (!inFile.is_open()) {
		std::cout << "inFile closed!" << std::endl;
		return;
	}
	
	if (inFile.eof()){
		std::cout << "inFile end!" << std::endl;
		return;
	}

	//先读取存储的RTP包长度
	unsigned int dataLen;
	inFile.read(reinterpret_cast<char*>(&dataLen), sizeof(dataLen));
	if (inFile.eof()) return;
	
	//读取一帧完整的RTP包
	std::vector<unsigned char> rtpData(dataLen);
	inFile.read((char*)rtpData.data(), dataLen);
	//rtp头长度包含扩展头
	int head_len = Base_RtpGetAllHeadSize(rtpData.data(), dataLen);
	//获取RTP包中的PT值
	unsigned char pt = Base_RtpGetPt(rtpData.data());


	switch (pt)
	{
	case 11: //pt为11rtp的负载中即为pcm数据
		{
			//存放pcm数据位置
			uint8_t* pcm_data = rtpData.data() + head_len;
			int pcm_len = dataLen - head_len - Base_RtpGetPaddingLen(rtpData.data(), dataLen);

			//直接播放pcm数据
			SDL_PutAudioStreamData(stream, pcm_data, pcm_len);
			break;
		}
	case 116://pt为116rtp的负载中为opus数据
	case 126://pt为126rtp的负载中为red包数据
		{
			//存放opus的数据位置
			uint8_t* opus_data;
			int opus_len;
			int ret;       //临时返回值
			if (pt == 116) {
				opus_data = rtpData.data() + head_len;
				opus_len = dataLen - head_len - Base_RtpGetPaddingLen(rtpData.data(), dataLen);
			}
			else {
				int reduant_len = redGetReduantLen(rtpData.data() + head_len); //计算实际负载数据前冗余数据总长度
				opus_data = rtpData.data() + head_len + reduant_len;
				opus_len = dataLen - head_len - reduant_len - Base_RtpGetPaddingLen(rtpData.data(), dataLen);
			}

			pkt->data = opus_data;
			pkt->size = opus_len;

			// 解码数据包
			ret = avcodec_send_packet(codecCtx, pkt);
			if (ret < 0) {
				std::cerr << "Error sending packet for decoding." << std::endl;
				avcodec_free_context(&codecCtx);
				av_frame_free(&frame);
				return;
			}

			while (ret >= 0) {
				ret = avcodec_receive_frame(codecCtx, frame);
				if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
					break;
				}
				else if (ret < 0) {
					std::cerr << "Error during decoding." << std::endl;
					avcodec_free_context(&codecCtx);
					av_frame_free(&frame);
					return;
				}

				//printf("Sample format: %s\n", av_get_sample_fmt_name((enum AVSampleFormat)frame->format));
				// 输出若为 flt 或 s32，则说明是32位格式
				//printf("Channels: %d\n", frame->ch_layout.nb_channels);
				// 正确应为1，若为2则配置错误

				// 转换每一帧,需要从fltp格式转为s16格式
				AVFrame* s16_frame = av_frame_alloc();
				s16_frame->format = AV_SAMPLE_FMT_S16;
				s16_frame->ch_layout.nb_channels = 1;
				s16_frame->sample_rate = 48000;
				s16_frame->nb_samples = frame->nb_samples;
				av_frame_get_buffer(s16_frame, 0);

				swr_convert(swr, s16_frame->data, s16_frame->nb_samples, (const uint8_t**)frame->data, frame->nb_samples);

				//播放重采样后的音频数据
				SDL_PutAudioStreamData(stream, (const void*)s16_frame->data[0], s16_frame->linesize[0]);

				av_frame_free(&s16_frame);
			}

			break;
		}
	default:
		std::cout << "unknow pt:" << pt << std::endl;
		return;
	}
}

int main(int argc, char* argv[]) {

	std::cout << "hello world!" << std::endl;
	//ffmpeg version : N - 117810 - g1912c86af6 - 20241117
	printf("ffmpeg version:%s\n", av_version_info());

	//We compiled against SDL version 3.2.4 ...
	const int compiled = SDL_VERSION;  /* hardcoded number from SDL headers */
	SDL_Log("We compiled against SDL version %d.%d.%d ...\n",
		SDL_VERSIONNUM_MAJOR(compiled),
		SDL_VERSIONNUM_MINOR(compiled),
		SDL_VERSIONNUM_MICRO(compiled));

	std::string mp4File = FindMp4File();

	std::cout << mp4File << std::endl;
	if (mp4File.empty()) {
		std::cerr << "未找到.mp4文件" << std::endl;
		SDL_Delay(2000);
		return 1;
	}

	inFile.open(mp4File, std::ios::binary);
	if (!inFile.is_open()) {
		std::cerr << "无法打开文件: " << mp4File << std::endl;
		SDL_Delay(2000);
		return 2;
	}

	//outFile.open("out.pcm", std::ios::binary);

	// 初始化SDL
	if (SDL_Init(SDL_INIT_AUDIO) < 0) {
		SDL_LogError(SDL_LOG_CATEGORY_AUDIO, "Init failed: %s", SDL_GetError());
		return -1;
	}
	//SDL播放音频参数
	SDL_AudioSpec spec;
	spec.format = SDL_AUDIO_S16LE;
	spec.channels = 1;
	spec.freq = 48000;

	// 获取默认输出设备
	SDL_AudioDeviceID devId = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec);
	if (!devId) {
		SDL_LogError(SDL_LOG_CATEGORY_AUDIO, "No audio device: %s", SDL_GetError());
		return -1;
	}


	// 创建音频流
	SDL_AudioStream* stream = SDL_OpenAudioDeviceStream(
		devId,
		&spec,
		AudioCallback,
		nullptr
	);
	if (!stream) {
		SDL_LogError(SDL_LOG_CATEGORY_AUDIO, "Open device failed: %s", SDL_GetError());
		return -1;
	}


	av_log_set_level(AV_LOG_TRACE);
	// 打开Opus解码器
	const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_OPUS);
	if (!codec) {
		std::cerr << "Could not find Opus codec." << std::endl;
		return -1;
	}
	codecCtx = avcodec_alloc_context3(codec);
	if (!codecCtx) {
		std::cerr << "Could not allocate codec context." << std::endl;
		return -1;
	}

	//codecCtx->sample_rate = 48000;
	//codecCtx->sample_fmt = AV_SAMPLE_FMT_S16;
	//codecCtx->request_sample_fmt = AV_SAMPLE_FMT_S16;
	//codecCtx->request_channel_layout = AV_CH_LAYOUT_MONO;
	//av_channel_layout_default(&codecCtx->ch_layout, 1); // 单声道

	if (avcodec_open2(codecCtx, codec, nullptr) < 0) {
		std::cerr << "Could not open codec." << std::endl;
		avcodec_free_context(&codecCtx);
		return -1;
	}

	// 分配音频帧
	frame = av_frame_alloc();
	if (!frame) {
		std::cerr << "Could not allocate audio frame." << std::endl;
		avcodec_free_context(&codecCtx);
		return -1;
	}

	// 分配输入包
	pkt = av_packet_alloc();

	//ffmpeg里面opus解码器解出的格式默认为AV_SAMPLE_FMT_FLTP格式，需要重采样成AV_SAMPLE_FMT_S16来进行播放
	swr = swr_alloc();
	av_opt_set_int(swr, "in_sample_rate", 48000, 0);
	av_opt_set_int(swr, "out_sample_rate", 48000, 0);
	av_opt_set_sample_fmt(swr, "in_sample_fmt", AV_SAMPLE_FMT_FLTP, 0);
	av_opt_set_sample_fmt(swr, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);
	AVChannelLayout ch_layout;
	av_channel_layout_default(&ch_layout, 1); // 单声道
	av_opt_set_chlayout(swr, "in_chlayout", &ch_layout, 0);
	av_opt_set_chlayout(swr, "out_chlayout", &ch_layout, 0);
	swr_init(swr);
	std::cout << "swr isInit:" << swr_is_initialized(swr) << std::endl;


	//启动播放
	SDL_ResumeAudioDevice(SDL_GetAudioStreamDevice(stream));

	//等待文件播放完毕
	while (!inFile.eof()) {
		SDL_Delay(1000);
		std::cout << "playing...." << std::endl;
	}

	inFile.close();

	//停止播放
	SDL_PauseAudioDevice(SDL_GetAudioStreamDevice(stream));

	// 销毁音频流并关闭设备
	SDL_DestroyAudioStream(stream);

	SDL_Quit();

	// 释放资源
	av_packet_free(&pkt);
	avcodec_send_packet(codecCtx, nullptr);
	avcodec_receive_frame(codecCtx, frame);
	avcodec_free_context(&codecCtx);
	av_frame_free(&frame);

	return 0;

	//int64_t current_audio_pts = av_rescale_q(1, { 1, 1000 }, { 1, 90000 });
	//std::cout << current_audio_pts << std::endl;

	//current_audio_pts = av_rescale_q(41, { 1, 1000 }, { 1, 90000 });
	//std::cout << current_audio_pts << std::endl;

	//int name;
	//std::cin >> name;

}

#endif
