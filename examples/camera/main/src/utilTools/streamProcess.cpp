#include"streamProcess.h"
#include"NPQosBundle.h" 
#include <fstream>
#include"peer.h"
#include <sstream>
//extern std::list<C_Peer_Inner *>     gs_aobjNetAdaptPeers;     /*全部的Peer通道列表*/

C_StreamProcess::C_StreamProcess(NETADAPTSFU_STREAMCFG_ST *pstStreamCfg)
	            :m_stStreamCfg(*pstStreamCfg),
				m_uiWidth(0),
				m_uiHeight(0),
				m_stStreamStatistic{},
			    m_stInputSave{},
				m_stOutputSave{},
				m_stIOStatistic{}
{

}


C_StreamProcess::~C_StreamProcess()
{
	if (m_stInputSave.ofFile.is_open())
	{
		m_stInputSave.ofFile.close();
	}

	if (m_stOutputSave.ofFile.is_open())
	{
		m_stOutputSave.ofFile.close();
	}
}

void C_StreamProcess::Rtp_Input_Statistic(unsigned char *pucDataBuf, unsigned int uiDataLen)
{
	unsigned char ucPt = Base_RtpGetPt(pucDataBuf);
	if (m_stStreamCfg.enStreamType == STREAM_VIDEO
		&& m_stStreamCfg.unStreamInfo.stVideoInfo.enFecType != NETADAPTSFU_FECTYPE_NONE
		&& kNpqFecPt == ucPt)
	{
		/*FEC冗余包统计*/
		rtxFecStatistic(&(m_stStreamStatistic.stInputFecInfo), pucDataBuf, uiDataLen, "Input");
		return;
	}

	/*RTP重传数据统计*/
	if (Base_RtpIsRtx(pucDataBuf, uiDataLen))
	{
		rtxFecStatistic(&(m_stStreamStatistic.stInputRtxInfo), pucDataBuf, uiDataLen, "Input");
		return;
	}

	/*预测冗余数据统计*/
	if (Base_RtpIsPadding(pucDataBuf, uiDataLen))
	{
		rtxFecStatistic(&(m_stStreamStatistic.stInputPadInfo), pucDataBuf, uiDataLen, "Input");
		return;
	}

	/*RTP包统计*/
	rtpStatistic(&(m_stStreamStatistic.stInputRtpInfo), pucDataBuf, uiDataLen, "Input");
	/*码流保存*/
	if (m_stInputSave.ofFile.is_open())
	{
		/*写入4字节RTP长度再写入RTP数据，研究院HikFileAnalyTool.exe工具分析*/
		m_stInputSave.ofFile.write((char*)&uiDataLen, sizeof(unsigned int));
		m_stInputSave.ofFile.write((char*)pucDataBuf, uiDataLen);
		m_stInputSave.uiNowFileSize += uiDataLen;
		/*文件大小达到保存设置大小时进行关闭*/
		if (m_stInputSave.uiNowFileSize >= m_stInputSave.uiFileSize)
		{
			m_stInputSave.ofFile.close();
		}
	}
}


void C_StreamProcess::Rtp_Output_Statistic(unsigned char *pucDataBuf, unsigned int uiDataLen)
{
	unsigned char ucPt = Base_RtpGetPt(pucDataBuf);
	if (m_stStreamCfg.enStreamType == STREAM_VIDEO
		&& m_stStreamCfg.unStreamInfo.stVideoInfo.enFecType != NETADAPTSFU_FECTYPE_NONE
		&& kNpqFecPt == ucPt)
	{
		rtxFecStatistic(&(m_stStreamStatistic.stOutputFecInfo), pucDataBuf, uiDataLen, "Output");   /*fec数据包统计*/
		return;
	}

	/*RTP重传数据统计*/
	if (Base_RtpIsRtx(pucDataBuf, uiDataLen))
	{
		rtxFecStatistic(&(m_stStreamStatistic.stOutputRtxInfo), pucDataBuf, uiDataLen, "Output");
		return;
	}

	/*预测冗余数据统计*/
	if (Base_RtpIsPadding(pucDataBuf, uiDataLen))
	{
		rtxFecStatistic(&(m_stStreamStatistic.stOutputPadInfo), pucDataBuf, uiDataLen, "Output");
		return;
	}

	/*RTP数据包统计*/
	rtpStatistic(&(m_stStreamStatistic.stOutputRtpInfo), pucDataBuf, uiDataLen, "Output");
	/*码流保存*/
	if (m_stOutputSave.ofFile.is_open())
	{
		/*写入4字节RTP长度再写入RTP数据，研究院HikFileAnalyTool.exe工具分析*/
		m_stOutputSave.ofFile.write((char*)&uiDataLen, sizeof(unsigned int));
		m_stOutputSave.ofFile.write((char*)pucDataBuf, uiDataLen);
		m_stOutputSave.uiNowFileSize += uiDataLen;
		/*文件大小达到保存设置大小时进行关闭*/
		if (m_stOutputSave.uiNowFileSize >= m_stOutputSave.uiFileSize)
		{
			m_stOutputSave.ofFile.close();
		}
	}
}


void C_StreamProcess::Rtcp_Input_Statistic(unsigned char *pucDataBuf, unsigned int uiDataLen)
{
	/*统计输入的RTCP数据*/
	Base_RtcpStatistic(&(m_stStreamStatistic.stInputRtcpInfo), pucDataBuf, uiDataLen);
}


void C_StreamProcess::Rtcp_Output_Statistic(unsigned char *pucDataBuf, unsigned int uiDataLen)
{
	/*统计输出的RTCP数据*/
	Base_RtcpStatistic(&(m_stStreamStatistic.stOutputRtcpInfo), pucDataBuf, uiDataLen);
}

/* RTP数据输入接口调用信息统计 */
void C_StreamProcess::Rtp_Input_IO_Statistic(unsigned int uiDelay, std::string info)
{
	iOStatistic(&m_stIOStatistic.stInputRtpIOInfo, uiDelay, info);
}

/* RTCP数据输入接口调用信息统计 */
void C_StreamProcess::Rtcp_Input_IO_Statistic(unsigned int uiDelay, std::string info)
{
	iOStatistic(&m_stIOStatistic.stInputRtcpIOInfo, uiDelay, info);
}

/* RTP数据输出接口调用信息统计 */
void C_StreamProcess::Rtp_Output_IO_Statistic(unsigned int uiDelay, std::string info)
{
	iOStatistic(&m_stIOStatistic.stOutputRtpIOInfo, uiDelay, info);
}

/* RTCP数据输出接口调用信息统计 */
void C_StreamProcess::Rtcp_Output_IO_Statistic(unsigned int uiDelay, std::string info)
{
	iOStatistic(&m_stIOStatistic.stOutputRtcpIOInfo, uiDelay, info);
}

void C_StreamProcess::SaveStreamSetParam(std::string strFilePath, unsigned int uiFileSize, std::string strSaveStep, bool bSave)
{
	/*取消保存时,默认取消所有正在保存的流，输入输出流都取消保存*/
	if (!bSave)
	{
		if (m_stInputSave.ofFile.is_open())
		{
			m_stInputSave.ofFile.close();
			m_stInputSave.uiFileSize = 0;
			m_stInputSave.uiNowFileSize = 0;
		}
		if (m_stOutputSave.ofFile.is_open())
		{
			m_stOutputSave.ofFile.close();
			m_stOutputSave.uiFileSize = 0;
			m_stOutputSave.uiNowFileSize = 0;
		}
		return;
	}

	/*设置输入或输出流保存句柄*/
	if (strSaveStep == "input" && !m_stInputSave.ofFile.is_open())
	{
		m_stInputSave.ofFile.open(strFilePath, std::ios::out | std::ios::binary);
		m_stInputSave.uiFileSize = uiFileSize * 1024 * 1024;  //传入文件大小单位为M，进行转换
		m_stInputSave.uiNowFileSize = 0;
	}

	if (strSaveStep == "output" && !m_stOutputSave.ofFile.is_open())
	{
		m_stOutputSave.ofFile.open(strFilePath, std::ios::out | std::ios::binary);
		m_stOutputSave.uiFileSize = uiFileSize * 1024 * 1024;
		m_stOutputSave.uiNowFileSize = 0;
	}

	return;
}

/** @fn PrintStreamStatistic
*   @brief  打印流状态统计
*   @param  pscSendBuf
*   @return
*/
int C_StreamProcess::PrintStreamStatistic(char *pscSendBuf)
{
	/* 对重要的接口调用耗时进行统计 */
	printIOStatisticInfo(pscSendBuf);

	/*获取输入的RTP统计*/
	printRtpStatisticInfo(true, pscSendBuf);
	/*获取输入的RTCP统计*/
	printRtcpStatisticInfo(true, pscSendBuf);

	/*获取NPQ相关状态统计*/
	NPQ_STAT stNpqStat;
	int siRetVal;
	/*获取对应的流的NPQ统计状态*/
	siRetVal = GetNpqStat(&stNpqStat);
	if (NPQ_OK == siRetVal)
	{
		sprintf(pscSendBuf + strlen(pscSendBuf),
			"**********************************************************Npq Quality*********************************************************\n");
		sprintf(pscSendBuf + strlen(pscSendBuf),
			"BitRate   TBitRate  RttUs     RealRttUs LossInput LossRecov FrameRate\n");
		sprintf(pscSendBuf + strlen(pscSendBuf),
			"%-10d%-10d%-10d%-10d%-10d%-10d%-10d\n",
			stNpqStat.nBitRate / 1000, stNpqStat.nTotalBitRate / 1000,  stNpqStat.nRttUs, stNpqStat.nRealRttUs,
			stNpqStat.cLossFraction, stNpqStat.cLossFraction2, stNpqStat.nFrameRate);

		if (m_stStreamCfg.enStreamType == STREAM_VIDEO)
		{
			sprintf(pscSendBuf + strlen(pscSendBuf),
				"VideoRate NackRate  FecRate   Delayms   JittIV    JittOV    PicQ      RttQ      FluQ      \n");
			sprintf(pscSendBuf + strlen(pscSendBuf),
				"%-10d%-10d%-10d%-10d%-10d%-10d%-10d%-10d%-10d\n",
				stNpqStat.nVideoBitRate / 1000, stNpqStat.nBitRateNack / 1000, stNpqStat.nBitRateFec / 1000, 
				stNpqStat.nVideoDelay / 1000, stNpqStat.nVideoJitterI / 1000, stNpqStat.nVideoJitterO / 1000,
				stNpqStat.nVideoPicQ, stNpqStat.nVideoRTQ, stNpqStat.nVideoFluQ);
		}
		else if (m_stStreamCfg.enStreamType == STREAM_AUDIO)
		{
			sprintf(pscSendBuf + strlen(pscSendBuf),
				"Delayms   JittIA    JittOA    TonQ      RttQ      FluQ      \n");
			sprintf(pscSendBuf + strlen(pscSendBuf),
				"%-10d%-10d%-10d%-10d%-10d%-10d\n",
				stNpqStat.nAudioDelay / 1000, stNpqStat.nAudioJitterI / 1000, stNpqStat.nAudioJitterO / 1000,
				stNpqStat.nAudioTonQ, stNpqStat.nAudioRTQ, stNpqStat.nAudioFluQ);
		}
	}
	else
	{
		sprintf(pscSendBuf + strlen(pscSendBuf), "NpqGetStat Failed, RetVal <0x%x>\n", siRetVal);
		NLOG_ERR("NpqGetStat Failed, RetVal <0x%x>\n", siRetVal);
	}

	/*获取输出的RTP统计*/
	printRtpStatisticInfo(false, pscSendBuf);
	/*获取输出的RTCP统计*/
	printRtcpStatisticInfo(false, pscSendBuf);

	return 0;
}

/* 打印接口调用IO统计情况 */
int C_StreamProcess::printIOStatisticInfo(char *pscSendBuf)
{
	NETADAPT_IO_RESULT_ST stResult;
	sprintf(pscSendBuf + strlen(pscSendBuf),
		"**********************************************************IOStatisticInfo*****************************************************\n");
	sprintf(pscSendBuf + strlen(pscSendBuf),
		"API            Max(us)-Seq     Min-----Seq     Avg             API            Max-----Type    Min-----Type    Avg   \n");

	stResult = getIOStatistic(&m_stIOStatistic.stInputRtpIOInfo);
	sprintf(pscSendBuf + strlen(pscSendBuf),
		"%-15s%-8d%-8s%-8d%-8s%-8d        ",
		"InputRtpData", stResult.uiMaxDelay, stResult.strMaxDelayInfo.c_str(), stResult.uiMinDelay, stResult.strMinDelayInfo.c_str(), stResult.uiAverageDelay);


	stResult = getIOStatistic(&m_stIOStatistic.stInputRtcpIOInfo);
	sprintf(pscSendBuf + strlen(pscSendBuf),
		"%-15s%-8d%-8s%-8d%-8s%-8d\n",
		"InputRtcpData", stResult.uiMaxDelay, stResult.strMaxDelayInfo.c_str(), stResult.uiMinDelay, stResult.strMinDelayInfo.c_str(), stResult.uiAverageDelay);

	stResult = getIOStatistic(&m_stIOStatistic.stOutputRtpIOInfo);
	sprintf(pscSendBuf + strlen(pscSendBuf),
		"%-15s%-8d%-8s%-8d%-8s%-8d        ",
		"OnOutputRtp", stResult.uiMaxDelay, stResult.strMaxDelayInfo.c_str(), stResult.uiMinDelay, stResult.strMinDelayInfo.c_str(), stResult.uiAverageDelay);

	stResult = getIOStatistic(&m_stIOStatistic.stOutputRtcpIOInfo);
	sprintf(pscSendBuf + strlen(pscSendBuf),
		"%-15s%-8d%-8s%-8d%-8s%-8d\n",
		"OnOutputRtcp", stResult.uiMaxDelay, stResult.strMaxDelayInfo.c_str(), stResult.uiMinDelay, stResult.strMinDelayInfo.c_str(), stResult.uiAverageDelay);

	return 0;
}


/* 打印RTP统计情况 */
int C_StreamProcess::printRtpStatisticInfo(bool bInput, char *pscSendBuf)
{
	NETADAPT_STATISTIC_RTP_ST *pstRtp = nullptr;
	NETADAPT_STATISTIC_RTP_ST *pstFec = nullptr;
	NETADAPT_STATISTIC_RTP_ST *pstRtx = nullptr;
	NETADAPT_STATISTIC_RTP_ST *pstPad = nullptr;
	if (bInput)
	{
		pstRtp = &(m_stStreamStatistic.stInputRtpInfo);
		pstFec = &(m_stStreamStatistic.stInputFecInfo);
		pstRtx = &(m_stStreamStatistic.stInputRtxInfo);
		pstPad = &(m_stStreamStatistic.stInputPadInfo);
		sprintf(pscSendBuf + strlen(pscSendBuf), 
			"**********************************************************RtpInputInfo********************************************************\n");
	}
	else
	{
		pstRtp = &(m_stStreamStatistic.stOutputRtpInfo);
		pstFec = &(m_stStreamStatistic.stOutputFecInfo);
		pstRtx = &(m_stStreamStatistic.stOutputRtxInfo);
		pstPad = &(m_stStreamStatistic.stOutputPadInfo);
		sprintf(pscSendBuf + strlen(pscSendBuf), 
			"**********************************************************RtpOutputInfo*******************************************************\n");
	}

	/*获取RTP相关的统计信息*/
	sprintf(pscSendBuf + strlen(pscSendBuf),
		"PT:chg         Ssrc:chg       Seq:chg        TS:chg         Size           FrameIntv      KeyCnt         \n");
	sprintf(pscSendBuf + strlen(pscSendBuf),
		"%-4u:%-10u%08x:%-6u%-7u:%-7u%08x:%-6u%-15d%-15d%-15d\n",
		pstRtp->uiLastPayloadType, pstRtp->uiPtChgCnt,        pstRtp->uiLastSsrc,      pstRtp->uiSsrcChgCnt,
		pstRtp->usLastSequence,    pstRtp->uiSeqChgCnt,       pstRtp->uiLastTimeStamp, pstRtp->uiTsChgCnt,   pstRtp->uiLastSize,
		pstRtp->uiSpaceTimeMs,     pstRtp->uiRecvKeyFrameCnt);

	/*视频通道开启fec时进行fec相关的统计打印*/
	if (m_stStreamCfg.enStreamType == STREAM_VIDEO && m_stStreamCfg.unStreamInfo.stVideoInfo.enFecType != NETADAPTSFU_FECTYPE_NONE)
	{
		sprintf(pscSendBuf + strlen(pscSendBuf),
			"PT:chg         Ssrc:chg       Seq:chg        TS:chg         Size           TimeMs         \n");
		sprintf(pscSendBuf + strlen(pscSendBuf),
			"%-4u:%-10u%08x:%-6u%-7u:%-7u%08x:%-6u%-15d%-15d\n",
			pstFec->uiLastPayloadType, pstFec->uiPtChgCnt,     pstFec->uiLastSsrc,     pstFec->uiSsrcChgCnt,
			pstFec->usLastSequence, pstFec->uiSeqChgCnt,       pstFec->uiLastTimeStamp, pstFec->uiTsChgCnt, pstFec->uiLastSize,
			pstFec->uiSpaceTimeMs);
	}

	/*获取RTP FEC RTX(重传数据) 相关数据的码率，以及RTP数据的帧抖动*/
	sprintf(pscSendBuf + strlen(pscSendBuf),
		"RtpCnt         Rtx    |Pad    FecCnt         RtpSec(kbps)   Rtx    |Pad    FecSec(kbps)   FrameJitt(ms)  \n");
	sprintf(pscSendBuf + strlen(pscSendBuf),
		"%-15d%-7d|%-7d%-15d%-15d%-7d|%-7d%-15d%-15d\n",
		pstRtp->uiTotalCnt,
		pstRtx->uiTotalCnt,
		pstPad->uiTotalCnt,
		pstFec->uiTotalCnt,
		recentSpeedShow(&pstRtp->stRecentSpeed) / 1000,
		recentSpeedShow(&pstRtx->stRecentSpeed) / 1000,
		recentSpeedShow(&pstPad->stRecentSpeed) / 1000,
		recentSpeedShow(&pstFec->stRecentSpeed) / 1000,
		recentFrameJitterShow(&pstRtp->stRecentFrameJitter));

	return 0;
}

/* 打印RTCP统计情况 */
int C_StreamProcess::printRtcpStatisticInfo(bool bInput, char *pscSendBuf)
{
	BASE_STATISTIC_RTCP_ST *pstRtcp = NULL;
	if (bInput)
	{
		pstRtcp = &(m_stStreamStatistic.stInputRtcpInfo);
		sprintf(pscSendBuf + strlen(pscSendBuf),
			"**********************************************************RtcpInputInfo*******************************************************\n");
	}
	else
	{
		pstRtcp = &(m_stStreamStatistic.stOutputRtcpInfo);
		sprintf(pscSendBuf + strlen(pscSendBuf),
			"**********************************************************RtcpOutputInfo******************************************************\n");
	}

	sprintf(pscSendBuf + strlen(pscSendBuf),
		"RtcpCnt     SSRC        RR      SR      Sends   XrRef   XrDlrr  Nack    TCC     Remb    PLI     FIR     App     Unknow  \n");
	sprintf(pscSendBuf + strlen(pscSendBuf),
		"%-12d0x%-8x  %-8d%-8d%-8d%-8d%-8d%-8d%-8d%-8d%-8d%-8d%-8d%-8d\n",
		pstRtcp->uiTotalCnt,     pstRtcp->uiSsrc,      pstRtcp->uiRRCnt,   pstRtcp->uiSRCnt,  pstRtcp->uiSedsCnt,
		pstRtcp->uiXrRefTimeCnt, pstRtcp->uiXrDlrrCnt, pstRtcp->uiNackCnt, pstRtcp->uiTCCCnt, pstRtcp->uiRembCnt,
		pstRtcp->uiPliCnt,       pstRtcp->uiFirCnt,    pstRtcp->uiAppCnt,  pstRtcp->uiUnknowCnt);

	return 0;
}

/*统计接口调用信息*/
void C_StreamProcess::iOStatistic(NETADAPT_IO_INFO_ST* stIOInfo, unsigned int uiDelay, std::string info)
{
	std::lock_guard<std::mutex> lock(stIOInfo->objMutex);
	stIOInfo->auiDelays[stIOInfo->uiNowIdx] = uiDelay;
	stIOInfo->strDelayInfos[stIOInfo->uiNowIdx] = info;
	stIOInfo->uiNowIdx = (stIOInfo->uiNowIdx + 1) & (MAX_IO_STATISTICS_NUM - 1);     /*使用与运算速度快，但数组列表的大小必须是2的幕数*/
	++stIOInfo->uiCounter;
}

/*获取接口统计信息*/
NETADAPT_IO_RESULT_ST C_StreamProcess::getIOStatistic(NETADAPT_IO_INFO_ST* stIOInfo)
{
	std::lock_guard<std::mutex> lock(stIOInfo->objMutex);
	NETADAPT_IO_RESULT_ST stResult{0};

	/*用于计算统计的个数*/
	unsigned int uiCnt = (stIOInfo->uiCounter > MAX_IO_STATISTICS_NUM) ? MAX_IO_STATISTICS_NUM : stIOInfo->uiCounter;
	if (uiCnt == 0)
	{
		return stResult;  /*暂时没接口调用记录*/
	}

	unsigned int uiTotalDelay = 0;
	stResult.uiMinDelay = 0x7fffffff; /*最小延时默认为一个大值*/
	/*逐个计算*/
	for (unsigned int i = 0; i<uiCnt; i++)
	{
		if (stResult.uiMaxDelay < stIOInfo->auiDelays[i])
		{
			stResult.uiMaxDelay = stIOInfo->auiDelays[i]; /*最大值*/
			stResult.strMaxDelayInfo = stIOInfo->strDelayInfos[i];
		}
		if (stResult.uiMinDelay > stIOInfo->auiDelays[i])
		{
			stResult.uiMinDelay = stIOInfo->auiDelays[i]; /*最小值*/
			stResult.strMinDelayInfo = stIOInfo->strDelayInfos[i];
		}
		uiTotalDelay += stIOInfo->auiDelays[i];  /*所以有延时的总和，用于后续计算平均值*/
	}

	stResult.uiAverageDelay = uiTotalDelay / uiCnt;     /*平均值*/

	return stResult;
}

/*统计RTP信息*/
void C_StreamProcess::rtpStatistic(NETADAPT_STATISTIC_RTP_ST* pstRtpInfo, unsigned char *pucDataBuf, unsigned int uiDataLen, const char* logInfo)
{
	const unsigned int    uiNow = Base_GetTimeTickMs();
	const unsigned char   uiPt = Base_RtpGetPt(pucDataBuf);
	const unsigned short  usSeq = Base_RtpGetSeq(pucDataBuf);
	const unsigned int    uiSsrc = Base_RtpGetSsrc(pucDataBuf);
	const unsigned int    uiTp = Base_RtpGetTimestamp(pucDataBuf);
	const bool            bMark = Base_RtpGetMark(pucDataBuf);

	if (0 == pstRtpInfo->uiTotalCnt)
	{
		pstRtpInfo->uiLastPayloadType = uiPt;
		pstRtpInfo->uiLastSsrc = uiSsrc;
		pstRtpInfo->usLastSequence = usSeq - 1;
		pstRtpInfo->uiLastTimeStamp = uiTp - 1;
		pstRtpInfo->bIframeInTP = false;    /*新时戳是否有I帧*/
		pstRtpInfo->uiLastTimeMs = uiNow;
		pstRtpInfo->bLastMark = true;
	}

	if (uiPt != pstRtpInfo->uiLastPayloadType)
	{
		pstRtpInfo->uiPtChgCnt++;
		NLOG_ERR("%s Pt Chg lastPt<%u> ---> Pt<%u> Seq<%u> Tp<%u> uiPtChgCnt<%d>", logInfo, pstRtpInfo->uiLastPayloadType, uiPt, usSeq, uiTp, pstRtpInfo->uiPtChgCnt);
		pstRtpInfo->uiLastPayloadType = uiPt;
	}

	if (uiSsrc != pstRtpInfo->uiLastSsrc)
	{
		pstRtpInfo->uiSsrcChgCnt++;
		NLOG_ERR("%s Ssrc Chg lastSsrc<%u> ---> Ssrc<%d> Seq<%u> Tp<%u> uiSsrcChgCnt<%d>", logInfo, pstRtpInfo->uiLastSsrc, uiSsrc, usSeq, uiTp, pstRtpInfo->uiSsrcChgCnt);
		pstRtpInfo->usLastSequence = usSeq - 1;        /*Seq为新数据*/
		pstRtpInfo->uiLastTimeStamp = uiTp - 1;        /**/
		pstRtpInfo->uiLastSsrc = uiSsrc;
	}

	
	bool bNewFrame = false; /*是否为新的一帧*/
	if (Base_RtpIsNewerSeq(usSeq, pstRtpInfo->usLastSequence)) /*过滤掉乱序包*/
	{
		const bool bNewTimeStamp = Base_RtpIsNewTimestamp(uiTp, pstRtpInfo->uiLastTimeStamp); /*新时戳的帧数据*/

		if (m_stStreamCfg.enStreamType == STREAM_AUDIO && bNewTimeStamp)                    /*音频新的seq中有新的时间戳则为新帧*/
			bNewFrame = true;
		else if(m_stStreamCfg.enStreamType != STREAM_AUDIO && pstRtpInfo->bLastMark == true) /*视频上一包为mark,这一包则为新帧*/
			bNewFrame = true;

		if (usSeq != (unsigned short)(pstRtpInfo->usLastSequence + 1))  /*seq是否连续*/
		{
			pstRtpInfo->uiSeqChgCnt++;
			pstRtpInfo->uiLastTimeStamp = uiTp;     /*记录新时戳*/
			NLOG_ERR("%s Seq Chg lastSeq<%u> ---> Seq<%u> Tp<%u> uiSeqChgCnt<%d>", logInfo, pstRtpInfo->usLastSequence, usSeq, uiTp, pstRtpInfo->uiSeqChgCnt);
		}
		else
		{
			/*视频seq连续且一帧之内时间戳不同*/
			if (m_stStreamCfg.enStreamType != STREAM_AUDIO && (!bNewFrame && pstRtpInfo->uiLastTimeStamp != uiTp) )
			{
				pstRtpInfo->uiTsChgCnt++;
				NLOG_ERR("%s Tp Chg OneFrame Inner, lastTp<%u> ---> Tp<%u> Seq<%u> uiTsChgCnt<%d>", logInfo, pstRtpInfo->uiLastTimeStamp, uiTp, usSeq, pstRtpInfo->uiTsChgCnt);
			}
			else if(m_stStreamCfg.enStreamType == STREAM_VIDEO && (bNewFrame && !bNewTimeStamp) ) /*新帧但是时间戳没有正向增长*/
			{
				pstRtpInfo->uiTsChgCnt++;
				NLOG_ERR("%s Tp Chg, New Frame But TimeStamp not New, lastTp<%u> ---> Tp<%u> Seq<%u> uiTsChgCnt<%d>", logInfo, pstRtpInfo->uiLastTimeStamp, uiTp, usSeq, pstRtpInfo->uiTsChgCnt);
			}
			else if (m_stStreamCfg.enStreamType == STREAM_AUDIO && !bNewTimeStamp) /*音频seq顺序增长，时间戳没有增长则统计时间戳变化*/
			{
				pstRtpInfo->uiTsChgCnt++;
				NLOG_ERR("%s Tp Chg lastTp<%u> ---> Tp<%u> Seq<%u> uiTsChgCnt<%d>", logInfo, pstRtpInfo->uiLastTimeStamp, uiTp, usSeq, pstRtpInfo->uiTsChgCnt);
			}
		}

		pstRtpInfo->usLastSequence = usSeq;
		pstRtpInfo->bLastMark = bMark;

		/*视频时，以mark认为一帧已经完整进行记录音频时，以新的时戳为一帧进行记录
		记录每帧时间，用于后续计算抖动*/
		if ((m_stStreamCfg.enStreamType != STREAM_AUDIO && bMark) || (m_stStreamCfg.enStreamType == STREAM_AUDIO && bNewFrame))
		{
			pstRtpInfo->stRecentFrameJitter.uiRecentFrameReceiveTime[pstRtpInfo->stRecentFrameJitter.uiNowIdx] = uiNow;
			pstRtpInfo->stRecentFrameJitter.uiNowIdx = (pstRtpInfo->stRecentFrameJitter.uiNowIdx + 1) % NETADAPT_STATISTIC_RECENT_FRAME_CNT;
			pstRtpInfo->uiSpaceTimeMs = uiNow - pstRtpInfo->uiLastTimeMs;/*两次帧之间的间隔时间*/
			pstRtpInfo->uiLastTimeMs = uiNow;                                 /*上一个帧的时间*/
		}
	}

	if (bNewFrame)
	{
		pstRtpInfo->uiLastTimeStamp = uiTp;     /*记录新时戳*/
		pstRtpInfo->bIframeInTP = false;        /*新时戳是否有I帧*/
	}

	pstRtpInfo->uiTotalCnt++;
	pstRtpInfo->uiLastSize = uiDataLen;



	/*分析并且记录I帧*/
	if (bNewFrame && m_stStreamCfg.enStreamType == STREAM_VIDEO && !pstRtpInfo->bIframeInTP)
	{
		/*I帧判断要区分H264和H265格式*/
		bool bVideoEncH265 = m_stStreamCfg.unStreamInfo.stVideoInfo.enVideoType == NETADAPTSFU_VIDEOTYPE_H265 ? true : false;

		/*此处的I帧类型检测不会对帧的完整性进行检测，例如，如果这个I帧只有后半段， 那么依然认为是有I帧的*/
		if (Base_RtpIsKeyFrame(pucDataBuf, uiDataLen, bVideoEncH265))
		{
			pstRtpInfo->uiRecvKeyFrameCnt++;                  /*I帧计数增加*/
			pstRtpInfo->bIframeInTP = true;               /*标记这个帧已经检测到I帧*/

			/*此处解析只有当sps在I帧最开头时才会进行分辨率解析，如果sps乱序到达，将不会进入该解析逻辑*/
			/* 解析码流中的分辨率用于上层显示,根据码流中的sps解析分辨率 */
			Base_RtpGetVideoResolution(&m_uiWidth, &m_uiHeight, pucDataBuf, uiDataLen, bVideoEncH265);
		}
	}

	/*记录瞬时带宽，用于后续计算码率*/
	recentSpeedRecode(&pstRtpInfo->stRecentSpeed, Base_GetTimeTickMs(), uiDataLen);

}

/*统计rtx重传冗余或fec冗余信息*/
void C_StreamProcess::rtxFecStatistic(NETADAPT_STATISTIC_RTP_ST* pstRtpInfo, unsigned char *pucDataBuf, unsigned int uiDataLen, const char* logInfo)
{
	const unsigned int    uiNow = Base_GetTimeTickMs();
	const unsigned char   uiPt = Base_RtpGetPt(pucDataBuf);
	const unsigned short  usSeq = Base_RtpGetSeq(pucDataBuf);
	const unsigned int    uiSsrc = Base_RtpGetSsrc(pucDataBuf);
	const unsigned int    uiTp = Base_RtpGetTimestamp(pucDataBuf);
	const bool            bMark = Base_RtpGetMark(pucDataBuf);

	if (0 == pstRtpInfo->uiTotalCnt)
	{
		pstRtpInfo->uiLastPayloadType = uiPt;
		pstRtpInfo->uiLastSsrc = uiSsrc;
		pstRtpInfo->usLastSequence = usSeq - 1;
		pstRtpInfo->uiLastTimeStamp = uiTp - 1;
		pstRtpInfo->bIframeInTP = false;    /*新时戳是否有I帧*/
		pstRtpInfo->uiLastTimeMs = uiNow;
		pstRtpInfo->bLastMark = true;
	}

	/* rtx包中有118和119py负载的空包，这里不进行pt跳变统计 */
	//if (uiPt != pstRtpInfo->uiLastPayloadType)
	//{
	//	NLOG_ERR("%s Pt Chg lastPt<%u> ---> Pt<%u> Seq<%u> Tp<%u>", logInfo, pstRtpInfo->uiLastPayloadType, uiPt, usSeq, uiTp);
	//	pstRtpInfo->uiPtChgCnt++;
	//	pstRtpInfo->uiLastPayloadType = uiPt;
	//}

	if (uiSsrc != pstRtpInfo->uiLastSsrc)
	{
		NLOG_ERR("%s Ssrc Chg lastSsrc<%u> ---> Ssrc<%d> Seq<%u> Tp<%u>", logInfo, pstRtpInfo->uiLastSsrc, uiSsrc, usSeq, uiTp);
		pstRtpInfo->uiSsrcChgCnt++;
		pstRtpInfo->usLastSequence = usSeq - 1;        /*Seq为新数据*/
		pstRtpInfo->uiLastTimeStamp = uiTp - 1;        /**/
		pstRtpInfo->uiLastSsrc = uiSsrc;
	}

	if (Base_RtpIsNewerSeq(usSeq, pstRtpInfo->usLastSequence)) /*过滤乱序包*/
	{
		pstRtpInfo->usLastSequence = usSeq;
		pstRtpInfo->bLastMark = bMark;
	}

	pstRtpInfo->uiLastTimeStamp = uiTp;     /*记录新时戳*/
	pstRtpInfo->uiTotalCnt++;
	pstRtpInfo->uiLastSize = uiDataLen;

	/*记录瞬时带宽，用于后续计算码率*/
	recentSpeedRecode(&pstRtpInfo->stRecentSpeed, Base_GetTimeTickMs(), uiDataLen);
}


/*记录瞬时数据量*/
void C_StreamProcess::recentSpeedRecode(NETADAPT_STATISTIC_RECENT_SPEED_ST *pstRecentSpeed, unsigned int uiNowTimeMs, unsigned int uiNowInputByte)
{
	NETADAPT_STATISTIC_RECENT_SPEED_NODE_ST *pstNowRecodeNode = &pstRecentSpeed->stRecentSpeedNode[pstRecentSpeed->uiNowCnt];

	/*是否为第一次进入统计*/
	if (pstNowRecodeNode->uiNowRecodeStartMs == 0)
	{
		pstNowRecodeNode->uiNowRecodeStartMs = uiNowTimeMs;
		pstNowRecodeNode->uiNowRecodeStopMs = uiNowTimeMs + 1000;

	}

	/*检查是否适合在当前节点中插入*/
	if (uiNowTimeMs > pstNowRecodeNode->uiNowRecodeStopMs)
	{
		/*标记该节点已经写满*/
		pstNowRecodeNode->bNodeFull = true;

		/*记录到下一个节点中*/
		pstRecentSpeed->uiNowCnt++;
		if (pstRecentSpeed->uiNowCnt >= NETADAPT_STATISTIC_RECENT_SEC)
		{
			pstRecentSpeed->uiNowCnt = 0;
		}
		pstNowRecodeNode = &pstRecentSpeed->stRecentSpeedNode[pstRecentSpeed->uiNowCnt];

		/*清空下一个节点，并在该节点中插入*/
		pstNowRecodeNode->bNodeFull = false;
		memset(pstNowRecodeNode, 0, sizeof(NETADAPT_STATISTIC_RECENT_SPEED_NODE_ST));
		pstNowRecodeNode->uiNowRecodeStartMs = uiNowTimeMs;
		pstNowRecodeNode->uiNowRecodeStopMs = uiNowTimeMs + 1000;
	}

	/*插入数据*/
	pstNowRecodeNode->uiNowRecodeBytes += uiNowInputByte;
	pstNowRecodeNode->uiLastRecTime = uiNowTimeMs;
}

/*获取最近几秒的平均码率*/
unsigned int C_StreamProcess::recentSpeedShow(NETADAPT_STATISTIC_RECENT_SPEED_ST *pstRecentSpeed)
{
	unsigned int uiIdx;
	unsigned int uiCostTimeCnt = 0;
	unsigned int uiBytes = 0;
	NETADAPT_STATISTIC_RECENT_SPEED_ST stRecentSpeed;
	NETADAPT_STATISTIC_RECENT_SPEED_NODE_ST *pstRecentSpeedNode;
	unsigned int uiNowTimeMs = Base_GetTimeTickMs();

	/*减小数据被并发修改的概率*/
	memcpy(&stRecentSpeed, pstRecentSpeed, sizeof(NETADAPT_STATISTIC_RECENT_SPEED_ST));

	/*统计时间*/
	for (uiIdx = 0; uiIdx < NETADAPT_STATISTIC_RECENT_SEC; uiIdx++)
	{
		pstRecentSpeedNode = &stRecentSpeed.stRecentSpeedNode[uiIdx];

		if ((true == pstRecentSpeedNode->bNodeFull) &&
			((uiNowTimeMs - pstRecentSpeedNode->uiNowRecodeStopMs) <= NETADAPT_STATISTIC_RECENT_SEC * 1000))
		{
			uiCostTimeCnt++;
			uiBytes += pstRecentSpeedNode->uiNowRecodeBytes;
		}
	}

	/*输出每秒时间*/
	if (0 == uiCostTimeCnt)
	{
		return 0;
	}
	else
	{
		return uiBytes * 8 / uiCostTimeCnt;
	}
}

/*获取最近范围帧之间的抖动情况*/
unsigned int C_StreamProcess::recentFrameJitterShow(NETADAPT_STATISTIC_RECENT_FRAME_ST *pstRecentFrame)
{
	unsigned int uiCnt = 0;
	unsigned int uiNowFrameTime = 0;
	unsigned int uiLastFrameTime = 0;
	unsigned int uiFrameJitter = 0;
	unsigned int uiFrameJitterMax = 0;
	NETADAPT_STATISTIC_RECENT_FRAME_ST stRecentFrame;
	unsigned int auiFrameInterval[NETADAPT_STATISTIC_RECENT_FRAME_CNT];

	/*减小数据被并发修改的概率*/
	memset(auiFrameInterval, 0x00, sizeof(auiFrameInterval));
	memcpy(&stRecentFrame, pstRecentFrame, sizeof(NETADAPT_STATISTIC_RECENT_FRAME_ST));

	unsigned int uiRIdx = stRecentFrame.uiNowIdx;
	do {
		uiLastFrameTime = stRecentFrame.uiRecentFrameReceiveTime[uiRIdx];
		uiRIdx = (uiRIdx + 1) % NETADAPT_STATISTIC_RECENT_FRAME_CNT;
	} while (0 == uiLastFrameTime && uiRIdx != stRecentFrame.uiNowIdx);

	if (uiRIdx == stRecentFrame.uiNowIdx)
	{
		return 0;
	}

	do {
		uiNowFrameTime = stRecentFrame.uiRecentFrameReceiveTime[uiRIdx];
		auiFrameInterval[uiCnt] = uiNowFrameTime - uiLastFrameTime;
		uiLastFrameTime = uiNowFrameTime;
		if (uiCnt > 1)
		{
			uiFrameJitter = abs((int)(auiFrameInterval[uiCnt] - auiFrameInterval[uiCnt - 1]));
			if (uiFrameJitterMax < uiFrameJitter)
			{
				uiFrameJitterMax = uiFrameJitter;
			}
		}

		uiCnt++;
		uiRIdx = (uiRIdx + 1) % NETADAPT_STATISTIC_RECENT_FRAME_CNT;
	} while (uiRIdx != stRecentFrame.uiNowIdx);

	return uiFrameJitterMax;
}

