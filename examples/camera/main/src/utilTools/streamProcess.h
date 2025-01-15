/**************************************************************************************
*  Filename:    streamProcess.h
*  Description: RTP RTCP流处理类,包括统计和保存
*  Author:      gengwenguan
*  Create:      2024-01-25
*  Modification history:
**************************************************************************************/
#ifndef __STREAM_PROCESS_H__
#define __STREAM_PROCESS_H__

#include "netadaptsfu.h"
#include "rtpbase.h"
#include "logAdapt.h"
#include "NPQosBundle.h"  
#include <mutex>
#include <array>
#include <string>
#include <fstream>

/*码流文件保存*/
typedef struct
{
	std::ofstream        ofFile;                 /*文件保存句柄*/
	unsigned int         uiFileSize;             /*设置写入的数据大小*/
	unsigned int         uiNowFileSize;          /*当前写入的数据大小*/
}NETADAPT_WRITEFILE_ST;


/*码流统计信息*/
#define NETADAPT_STATISTIC_RECENT_SEC  5 /*统计最近几秒的到达数据量*/
typedef struct
{
	bool    bNodeFull;          /*该节点是否已经记录满了数据*/
									/*时间间隔*/
	unsigned int  uiNowRecodeStartMs;
	unsigned int  uiNowRecodeStopMs;
	unsigned int  uiLastRecTime;      /*最后记录的时间*/
	unsigned int  uiNowRecodeBytes;
}NETADAPT_STATISTIC_RECENT_SPEED_NODE_ST;

typedef struct
{
	unsigned int uiNowCnt;  /*当前写入的节点*/
	NETADAPT_STATISTIC_RECENT_SPEED_NODE_ST stRecentSpeedNode[NETADAPT_STATISTIC_RECENT_SEC];
}NETADAPT_STATISTIC_RECENT_SPEED_ST;

#define NETADAPT_STATISTIC_RECENT_FRAME_CNT  128    
typedef struct
{
	unsigned int uiNowIdx;  /*当前写入的节点*/
	unsigned int uiRecentFrameReceiveTime[NETADAPT_STATISTIC_RECENT_FRAME_CNT];
}NETADAPT_STATISTIC_RECENT_FRAME_ST;

/*媒体统计信息*/
typedef struct
{
	unsigned int     uiTotalCnt;
	unsigned int     uiLastPayloadType;
	unsigned int     uiLastSsrc;
	unsigned short   usLastSequence;
	unsigned int     uiLastSize;
	bool             bLastMark;

	unsigned int     uiLastTimeMs; /*上一帧的时间，以mark为标记*/
	unsigned int     uiSpaceTimeMs;/*最新两帧之间的间隔时间*/

	unsigned int     uiLastTimeStamp;  /*音视频数据时间戳*/
	bool             bIframeInTP;      /*新时戳中是否有I帧*/

	unsigned int     uiSsrcChgCnt;  /*SSRC发生切换的次数*/
	unsigned int     uiPtChgCnt;    /*pt发生切换的次数*/
	unsigned int     uiSeqChgCnt;   /*seq发生切换的次数*/
	unsigned int     uiTsChgCnt;    /*seq正常的情况下TS回跳的次数*/

	unsigned int     uiRecvKeyFrameCnt;     /*接收到的关键帧的数量*/

	/*统计最近几秒的到达数据量，用于计算码率*/
	NETADAPT_STATISTIC_RECENT_SPEED_ST stRecentSpeed;

	/*统计帧之间的抖动情况*/
	NETADAPT_STATISTIC_RECENT_FRAME_ST stRecentFrameJitter;

}NETADAPT_STATISTIC_RTP_ST;


/*RTP和RTCP相关统计信息*/
typedef struct
{
	/*输入的rtp，fec，rtc，padding，码率控制信息*/
	NETADAPT_STATISTIC_RTP_ST    stInputRtpInfo;
	NETADAPT_STATISTIC_RTP_ST    stInputFecInfo;
	NETADAPT_STATISTIC_RTP_ST    stInputRtxInfo;
	NETADAPT_STATISTIC_RTP_ST    stInputPadInfo;

	/*输出的rtp，fec，rtc，padding，码率控制信息*/
	NETADAPT_STATISTIC_RTP_ST    stOutputRtpInfo;
	NETADAPT_STATISTIC_RTP_ST    stOutputFecInfo;
	NETADAPT_STATISTIC_RTP_ST    stOutputRtxInfo;
	NETADAPT_STATISTIC_RTP_ST    stOutputPadInfo;

	/*
	rtcp消息统计
	*/
	BASE_STATISTIC_RTCP_ST   stInputRtcpInfo;
	BASE_STATISTIC_RTCP_ST   stOutputRtcpInfo;

}NETADAPT_STREAM_INFO_STATISTIC_ST;

#define MAX_IO_STATISTICS_NUM  256                   /*接口调用统计最新的总次数,需要定义为2的幂数, 方便快速运算*/
/*接口的调用信息*/
typedef struct
{
	std::mutex   objMutex{};
	unsigned int uiCounter{};                                 /*记录总数*/
	unsigned int uiNowIdx{};                                  /*当前记录的下标*/
	//unsigned int auiDelays[MAX_IO_STATISTICS_NUM];          /*接口调用的延时时间us*/
	std::array<unsigned int, MAX_IO_STATISTICS_NUM> auiDelays{};
	//std::string  strDelayInfos[MAX_IO_STATISTICS_NUM];      /*每个RTP延时时间对应的seq,或每个rtcp对应的类型*/
	std::array<std::string, MAX_IO_STATISTICS_NUM> strDelayInfos{};

}NETADAPT_IO_INFO_ST;

/*接口的调用信息计算的统计结果*/
typedef struct
{
	unsigned int uiMaxDelay;              /*接口调用最大延时*/
	std::string  strMaxDelayInfo;         /*接口调用最大延时对应的数据包信息*/
	unsigned int uiMinDelay;              /*接口调用最小延时*/
	std::string  strMinDelayInfo;         /*接口调用最小延时对应的数据包信息*/
	unsigned int uiAverageDelay;          /*接口调用的平均延时时间*/

}NETADAPT_IO_RESULT_ST;

/*接口的调用统计信息*/
typedef struct
{
	/* RTP RTCP输入接口统计 */
	NETADAPT_IO_INFO_ST    stInputRtpIOInfo{};
	NETADAPT_IO_INFO_ST    stInputRtcpIOInfo{};

	/* RTP RTCP输出接口统计 */
	NETADAPT_IO_INFO_ST    stOutputRtpIOInfo{};
	NETADAPT_IO_INFO_ST    stOutputRtcpIOInfo{};

}NETADAPT_IO_STATISTIC_ST;


/*流处理对象,包括流的统计和保存*/
class C_StreamProcess : virtual public C_LogAdapt
{
public:
	static constexpr unsigned int kNpqRedPt = 126;  //NPQ音频red包pt值
	static constexpr unsigned int kNpqFecPt = 117;  //NPQ音频fec冗余包pt值
public:
	C_StreamProcess(NETADAPTSFU_STREAMCFG_ST *pstStreamCfg);
	virtual ~C_StreamProcess();

	/** @fn MediaSaveSetParam
	*   @brief  媒体保存参数设置
	*   @param  strFilePath     [IN]  保存的文件路径（包含文件名）
	*			uiFileSize      [IN]  文件大小
	*			strSaveStep     [IN]  资源入口位置（输入\输出）
	*			bSave           [IN]  码流保存开关（false时停止保存）
	*   @return
	*/
	void SaveStreamSetParam(std::string strFilePath, unsigned int uiFileSize, std::string strSaveStep, bool bSave);

	/*打印流输入输出的详细统计信息*/
	int PrintStreamStatistic(char *pscSendBuf);

	/*获取数据包最后的输入\输出时间*/
	unsigned int GetLastInputTimeMs() { return m_stStreamStatistic.stInputRtpInfo.uiLastTimeMs; }
	unsigned int GetLastOutputTimeMs() { return m_stStreamStatistic.stOutputRtpInfo.uiLastTimeMs; }

	/*获取送入和吐出的RTP数据包数量，其中不包括FEC和RTX重传预测包*/
	int GetRtpInputCnt() { return m_stStreamStatistic.stInputRtpInfo.uiTotalCnt; }
	int GetRtpOutputCnt() { return m_stStreamStatistic.stOutputRtpInfo.uiTotalCnt; }

	/* 获取从码流中统计到的分辨率，视频流有效 */
	int GetResolutionWidth() { return m_uiWidth; }
	int GetResolutionHeight() { return m_uiHeight; }

protected:
	/* 获取NPQ的相关统计状态信息，需在子类C_Npq_Stream中实现 */
	virtual int GetNpqStat(NPQ_STAT* pstNpqStat) { return 0; }

	/*RTP、RTCP数据包输入、输出信息统计*/
	void Rtp_Input_Statistic(unsigned char *pucDataBuf, unsigned int uiDataLen);
	void Rtp_Output_Statistic(unsigned char *pucDataBuf, unsigned int uiDataLen);
	void Rtcp_Input_Statistic(unsigned char *pucDataBuf, unsigned int uiDataLen);
	void Rtcp_Output_Statistic(unsigned char *pucDataBuf, unsigned int uiDataLen);

	/* RTP、RTCP数据输入、输出接口调用信息统计 */
	void Rtp_Input_IO_Statistic(unsigned int uiDelay, std::string info);
	void Rtcp_Input_IO_Statistic(unsigned int uiDelay, std::string info);
	void Rtp_Output_IO_Statistic(unsigned int uiDelay, std::string info);
	void Rtcp_Output_IO_Statistic(unsigned int uiDelay, std::string info);


private:
	/*统计RTP信息*/
	void rtpStatistic(NETADAPT_STATISTIC_RTP_ST* pstRtpInfo, unsigned char *pucDataBuf, unsigned int uiDataLen, const  char* logInfo);

	/*统计rtx重传冗余或fec冗余信息*/
	void rtxFecStatistic(NETADAPT_STATISTIC_RTP_ST* pstRtpInfo, unsigned char *pucDataBuf, unsigned int uiDataLen, const  char* logInfo);

	/*统计接口调用信息*/
	void iOStatistic(NETADAPT_IO_INFO_ST* stIOInfo, unsigned int uiDelay, std::string info);

	/*获取接口统计信息*/
	NETADAPT_IO_RESULT_ST getIOStatistic(NETADAPT_IO_INFO_ST* stIOInfo);

	/*记录瞬时数据量*/
	void recentSpeedRecode(NETADAPT_STATISTIC_RECENT_SPEED_ST *pstRecentSpeed, unsigned int uiNowTimeMs, unsigned int uiNowInputByte);

	/*获取最近几秒的平均码率*/
	unsigned int recentSpeedShow(NETADAPT_STATISTIC_RECENT_SPEED_ST *pstRecentSpeed);

	/*获取最近范围帧之间的抖动情况*/
	unsigned int recentFrameJitterShow(NETADAPT_STATISTIC_RECENT_FRAME_ST *pstRecentFrame);

	/* 打印接口调用IO统计情况 */
	int printIOStatisticInfo(char *pscSendBuf);

	/* 打印RTP统计情况 */
	int printRtpStatisticInfo(bool bInput, char *pscSendBuf);

	/* 打印RTCP统计情况 */
	int printRtcpStatisticInfo(bool bInput, char *pscSendBuf);

private:

	NETADAPTSFU_STREAMCFG_ST             m_stStreamCfg;

	/*流输入输出RTP/RTCP统计信息*/
	NETADAPT_STREAM_INFO_STATISTIC_ST    m_stStreamStatistic;

	/*输入输出数据接口的调用统计信息*/
	NETADAPT_IO_STATISTIC_ST             m_stIOStatistic;

	/*输入输出流保存*/
	NETADAPT_WRITEFILE_ST                m_stInputSave;
	NETADAPT_WRITEFILE_ST                m_stOutputSave;

	unsigned int                         m_uiWidth;  /* 视频流的分辨率 */
	unsigned int                         m_uiHeight;
};

#endif

