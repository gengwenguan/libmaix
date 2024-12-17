#include <stdio.h>
#include <chrono>
#include <string>
#include <cstring>
#include <cstddef>
#if (defined(_WIN32) || defined(_WIN32_WCE) || defined(WIN64))
#include <winsock.h>  
#include <windows.h>
#define HPR_DELTA_EPOCH_IN_USEC   unsigned long long(11644473600000000)
#define HPR_US_PER_SEC 1000000
#define HPR_MS_PER_SEC 1000
#else
#include <arpa/inet.h>
#include <sys/time.h>
#endif

#include "rtpBase.h"

/**
* Base_TimeNowUs get now time  //使用对应平台的系统接口获取精准的UTC至今的微妙数
* @return number of microsecond since January 1, 1970.
* @sa
*/
unsigned long long Base_TimeNowUs()
{
	unsigned long long nTime = 0;
#if (defined (_WIN32) || defined (WIN64))
	LARGE_INTEGER lf;
	if (QueryPerformanceFrequency(&lf))
	{
		if (lf.QuadPart == HPR_MS_PER_SEC)//not support high performance timer.
		{
			FILETIME fileTime;
			GetSystemTimeAsFileTime(&fileTime);
			nTime = fileTime.dwHighDateTime;
			nTime = (nTime) << 32;
			nTime |= fileTime.dwLowDateTime;
			nTime /= 10;    /* Convert from 100 nano-sec periods to micro-seconds. */
			nTime -= HPR_DELTA_EPOCH_IN_USEC;
		}
		else
		{
			LARGE_INTEGER lc;
			if (QueryPerformanceCounter(&lc))
			{
				//offside ???
				nTime = lc.QuadPart;
				nTime *= HPR_US_PER_SEC;
				nTime /= (unsigned long long)lf.QuadPart;
			}
		}
	}
#else
	timeval tv;
	gettimeofday(&tv, NULL);
	nTime = tv.tv_sec;
	nTime *= 1000000;
	nTime += tv.tv_usec;
#endif
	return nTime;
}
/**
* Base_TimeNow get now time //使用c++标准时间接口获取的时间可能不精准
* @return number of microsecond since January 1, 1970.
* @sa 
*/
unsigned long long Base_TimeNow() 
{
	// 获取当前时间（自1970年1月1日至今的时间戳微秒数）   
	auto now = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count();

	return static_cast<unsigned long long>(now); // 返回微秒数（强制类型转换为long long类型）   
}
/**
* Base_GetTimeTickMs get now time
* @return number of milliseconds since system start. cpu time.
* @sa 
*/
unsigned int Base_GetTimeTickMs() 
{
	// 获取当前时间  
	auto now = std::chrono::system_clock::now();

	// 获取Epoch的时间点  
	auto epoch = now - std::chrono::milliseconds(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());

	// 计算当前时间距离Epoch的毫秒数  
	auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now - epoch).count();

	return static_cast<unsigned int>(millis); //返回毫秒数（强制类型转换为unsigned int类型）  
}

/*获取数据的具体类型，是RTP还是RTCP*/
BASE_DATA_TYPE Base_GetDataType(unsigned char *pucBuffer, unsigned int uiDataLen)
{
	if(uiDataLen >= 12 &&
	    (pucBuffer[0] > 127 && pucBuffer[0] < 192) &&
	    (pucBuffer[1] >= 192 && pucBuffer[1] <= 223) )
	{
		return BASE_RTCP;
	}

	if (uiDataLen >= 12 &&
		(pucBuffer[0] > 127 && pucBuffer[0] < 192) )
	{
		return BASE_RTP;
	}

	return BASE_UNKNOW;
}

/*获取RTP或者RTCP中的ssrc*/
unsigned int Base_DataGetSsrc(unsigned char *pucBuffer, unsigned int uiDataLen)
{
	BASE_DATA_TYPE emType = Base_GetDataType(pucBuffer, uiDataLen);

	if (BASE_RTCP == emType)
	{
		return Base_RtcpGetSsrc(pucBuffer);
	}

	if (BASE_RTP == emType)
	{
		return Base_RtpGetSsrc(pucBuffer);
	}

	return 0;
}

/*获取时间差*/
unsigned int Base_GetTimerPeriod(unsigned int uiStartMs, unsigned int uiStopMs)
{
    if(uiStartMs <= uiStopMs)
    {
        /*正常情况*/
        return uiStopMs - uiStartMs;
    }
    else
    {
        /*到达极限回退的情况*/
        return 0xFFFFFFFF - uiStartMs + uiStopMs;
    }
}


/*获取RTCP包的PT值*/
unsigned char Base_RtcpGetPt(unsigned char *pucRtcpBuffer)
{
    return *(unsigned char *)(&pucRtcpBuffer[1]);
}

unsigned int Base_RtcpGetSsrc(unsigned char *pucRtcpBuffer)
{
    return ntohl(*(unsigned int *)(&pucRtcpBuffer[4]));
}

void Base_RtcpSetSsrc(unsigned char *pucRtcpBuffer, unsigned int uiDataLen, unsigned int uiSsrc)
{    
    unsigned int uiNowRtcpOffSet = 0;    
    unsigned char *pucNowRtcpBuffer;
    /*后面包的ssrc也需要修改为相同的ssrc     */
    while(uiNowRtcpOffSet < uiDataLen)
    {
        pucNowRtcpBuffer = (unsigned char *)pucRtcpBuffer + uiNowRtcpOffSet;

        /*修改当前rtcp包的Ssrc*/        
        *(unsigned int *)(&pucNowRtcpBuffer[4]) = htonl(uiSsrc); 
        /*偏移至下一个ssrc*/
        uiNowRtcpOffSet += (Base_RtcpGetLen(pucNowRtcpBuffer) + 4);
    }
}

/*获取RTCP包的SUB PT值*/
unsigned char Base_RtcpGetSubPt(unsigned char *pucRtcpBuffer)
{
    return (*(unsigned char *)(&pucRtcpBuffer[0])) & 0x1F;
}

/*获取RTP包的Ver值*/
unsigned char Base_RtpGetVer(unsigned char *pucRtpBuffer)
{
    return ((*(unsigned char *)(&pucRtpBuffer[0])) >> 6) & 0x3;
}

/*获取RTP包头中的Mark值*/
bool Base_RtpGetMark(unsigned char *pucRtpBuffer)
{
    return ((pucRtpBuffer[1] & 0x80) == 0) ? false : true;    /*Mark位*/
}

/*配置RTP包头中的Mark值*/
void Base_RtpSetMark(unsigned char *pucRtpBuffer, bool bMark)
{
    pucRtpBuffer[1] = (pucRtpBuffer[1] & 0x7f) | (bMark ? 0x80 : 0x00);
}

/*获取RTP包头中的Extension值*/
bool Base_RtpGetExtension(unsigned char *pucRtpBuffer)
{
    return ((pucRtpBuffer[0] & 0x10) == 0) ? false : true;    /*Extension位*/
}

/*配置RTP包头中的Extension值*/
void Base_RtpSetExtension(unsigned char *pucRtpBuffer, bool bExtension)
{
    pucRtpBuffer[0] = (pucRtpBuffer[0] & 0xef) | (bExtension ? 0x10 : 0x00);
}

/*获取RTP包头中的Padding值*/
bool Base_RtpGetPadding(unsigned char *pucRtpBuffer)
{
    return ((pucRtpBuffer[0] & 0x20) == 0) ? false : true;    /*Padding位*/
}

/*配置RTP包头中的Padding值*/
void Base_RtpSetPadding(unsigned char *pucRtpBuffer, bool bPadding)
{
    pucRtpBuffer[0] = (pucRtpBuffer[0] & 0xdf) | (bPadding ? 0x20 : 0x00);
}

/*获取RTP包头中的Padding长度*/
unsigned int Base_RtpGetPaddingLen(unsigned char *pucRtpBuffer, unsigned int uiRtpLen)
{
    if(Base_RtpGetPadding(pucRtpBuffer))
    {
        return (unsigned int)pucRtpBuffer[uiRtpLen - 1];
    }
        
    return 0;
}

/*删除RTP包头中的Padding位并且去除实际负载中的padding,该函数会修改数据包的长度*/
void Base_RtpRmPadding(unsigned char *pucRtpBuffer, unsigned int& uiRtpLen)
{
    if(Base_RtpGetPadding(pucRtpBuffer))
    {
        uiRtpLen = uiRtpLen - (unsigned int)pucRtpBuffer[uiRtpLen - 1];
        Base_RtpSetPadding(pucRtpBuffer, false);
    }
}

/*获取RTP包的PT值*/
unsigned char Base_RtpGetPt(unsigned char *pucRtpBuffer)
{
    return (*(unsigned char *)(&pucRtpBuffer[1])) & 0x7F;
}

/*设置RTP包的PT值*/
void Base_RtpSetPt(unsigned char *pucRtpBuffer, unsigned char ucPayloadType)
{
    pucRtpBuffer[1] = (pucRtpBuffer[1] & 0x80) | ucPayloadType;
}

/*获取RTP包的时间戳*/
unsigned int Base_RtpGetTimestamp(unsigned char *pucRtpBuffer)
{
    return ntohl(*(unsigned int *)(&pucRtpBuffer[4]));
}

/*设置RTP TimeStamp*/
void Base_RtpSetTimestamp(unsigned char *pucRtpBuffer, unsigned int uiTimestamp)
{
	*(unsigned int *)(&pucRtpBuffer[4]) = htonl(uiTimestamp);
}

/*判断RTP 是否为新的时间戳*/
bool Base_RtpIsNewTimestamp(unsigned int uiTs, unsigned int uiPreTs)
{
    const unsigned int kBreakpoint = 0x80000000;
    const unsigned int uiSeqDiff   = uiTs - uiPreTs;

    if (uiSeqDiff == kBreakpoint)
    {
        return  (uiTs > uiPreTs)  ? true : false;
    }
    return (uiTs != uiPreTs && uiSeqDiff < kBreakpoint) ? true : false;
}


/*获取RTP包的ssrc*/
unsigned int Base_RtpGetSsrc(unsigned char *pucRtpBuffer)
{
    return ntohl(*(unsigned int *)(&pucRtpBuffer[8]));
}

/*获取RTP包的seq号*/
unsigned short Base_RtpGetSeq(unsigned char *pucRtpBuffer)
{
    return ntohs(*(unsigned short *)(&pucRtpBuffer[2]));
}

/*设置RTP包的seq号*/
void Base_RtpSetSeq(unsigned char *pucRtpBuffer, unsigned short sequence)
{
    *(unsigned short *)(&pucRtpBuffer[2]) = htons(sequence);
}

bool Base_RtpIsNewerSeq(unsigned short usSeq, unsigned short usPreSeq)
{
    const unsigned short kBreakpoint = 0x8000;
    const unsigned short usSeqDiff   = usSeq - usPreSeq;

    if (usSeqDiff == kBreakpoint)
    {
        return  (usSeq > usPreSeq) ? true : false;
    }

    return  (usSeq != usPreSeq && usSeqDiff < kBreakpoint) ? true : false;
}

void Base_RtpSetSsrc(unsigned char *pucRtpBuffer, unsigned int uiSsrc)
{
    *(unsigned int *)(&pucRtpBuffer[8]) = htonl(uiSsrc);
}

unsigned char Base_RtcpGetSubBt(unsigned char *pucRtcpBuffer)
{
    return (*(unsigned char *)(&pucRtcpBuffer[8]));
}

unsigned short Base_RtcpGetLen(unsigned char *pucRtcpBuffer)
{
    return ntohs(*(unsigned short *)(&pucRtcpBuffer[2])) * 4;
}

BASE_RTCP_TYPE Base_RtcpGetType(unsigned char *pucRtcpBuffer)
{
    unsigned int  uiType, uiSubType;
    unsigned int  uiBt;
    uiType    = Base_RtcpGetPt(pucRtcpBuffer);
    uiSubType = Base_RtcpGetSubPt(pucRtcpBuffer);
    uiBt      = Base_RtcpGetSubBt(pucRtcpBuffer);

    /*解析RTCP包*/
    switch(uiType)
    {
        case BASE_RTCP_PT_SR: return BASE_RTCP_SR_INFO;
        case BASE_RTCP_PT_RR: return BASE_RTCP_RR_INFO;
        case BASE_RTCP_PT_XR:
            switch(uiBt)
            {
                case BASE_RTCP_PT_XR_BT_REFTIME:  return BASE_RTCP_XR_REFERENCETIME_INFO;
                case BASE_RTCP_PT_XR_BT_DLRR:     return BASE_RTCP_XR_DLRR_INFO;
                default:  return BASE_RTCP_INVAILED;
            }
        case BASE_RTCP_PT_RTPFB:
            switch(uiSubType)
            {
                case BASE_RTCP_PT_RTPFB_SUBPT_NACK:  return BASE_RTCP_FB_NACK_INFO;
                case BASE_RTCP_PT_RTPFB_SUBPT_TCC:   return BASE_RTCP_FB_TRANSPORT_INFO;
                default:  return BASE_RTCP_INVAILED;
            }
            break;
        case BASE_RTCP_PT_PSFB:
            switch(uiSubType)
            {
                case BASE_RTCP_PT_PSFB_SUBPT_PLI:  return BASE_RTCP_SPEC_FB_PLI_INFO;
                case BASE_RTCP_PT_PSFB_SUBPT_FIR:  return BASE_RTCP_SPEC_FB_FIR_INFO;
                case BASE_RTCP_PT_PSFB_SUBPT_REMB: return BASE_RTCP_SPEC_FB_REMB_INFO;
                default:  return BASE_RTCP_INVAILED;
            }
        case BASE_RTCP_PT_SEDS: return BASE_RTCP_SEDS_INFO;
        case BASE_RTCP_PT_APP:  return BASE_RTCP_APP;
        default:  return BASE_RTCP_INVAILED;
    }

}

const char* Base_RtcpGetStrType(unsigned char *pucRtcpBuffer)
{
	unsigned int  uiType, uiSubType;
	unsigned int  uiBt;
	uiType = Base_RtcpGetPt(pucRtcpBuffer);
	uiSubType = Base_RtcpGetSubPt(pucRtcpBuffer);
	uiBt = Base_RtcpGetSubBt(pucRtcpBuffer);

	/*解析RTCP包*/
	switch (uiType)
	{
	case BASE_RTCP_PT_SR: return "SR";
	case BASE_RTCP_PT_RR: return "RR";
	case BASE_RTCP_PT_XR:
		switch (uiBt)
		{
		case BASE_RTCP_PT_XR_BT_REFTIME:  return "REF";
		case BASE_RTCP_PT_XR_BT_DLRR:     return "DLRR";
		default:  return "Unknow";
		}
	case BASE_RTCP_PT_RTPFB:
		switch (uiSubType)
		{
		case BASE_RTCP_PT_RTPFB_SUBPT_NACK:  return "NACK";
		case BASE_RTCP_PT_RTPFB_SUBPT_TCC:   return "TCC";
		default:  return "Unknow";
		}
		break;
	case BASE_RTCP_PT_PSFB:
		switch (uiSubType)
		{
		case BASE_RTCP_PT_PSFB_SUBPT_PLI:  return "PLI";
		case BASE_RTCP_PT_PSFB_SUBPT_FIR:  return "FIR";
		case BASE_RTCP_PT_PSFB_SUBPT_REMB: return "REMB";
		default:  return "Unknow";
		}
	case BASE_RTCP_PT_SEDS: return "SENDS";
	case BASE_RTCP_PT_APP:  return "APP";
	default:  return "Unknow";
	}
}


unsigned int RtcpComm_IsForSender(unsigned char *pscRtcpBuffer, bool *pbSender)
{
    /*确认是否为APP*/    
    if(Base_RtcpGetPt(pscRtcpBuffer) != BASE_RTCP_PT_APP)
    {
        return BASE_RTCP_INVAILED;
    }
    else
    {
        bool bSender = ((pscRtcpBuffer[12] & 0x01) == 0) ? true : false;
        bool bReply  = (((pscRtcpBuffer[12] >> 1) & 0x01) == 1) ? true : false;

        if(true == bSender)
        {
            /*源为发送端，如果为应答报文，则当前为发送端，如果为命令报文，则为接收端*/
            *pbSender = bReply;
        }
        else
        {
            /*源为接收端，如果为应答报文，则当前为接收端，如果为命令报文，则为发送端*/
            *pbSender = (true == bReply)?false:true;
        }
        return 0;
    }
}


BASE_RTCPROLE_EN Base_RtcpGetRoleByType(BASE_RTCP_TYPE enRtcpType, unsigned char *pscRtcpBuf)
{
    switch(enRtcpType)
    {
        /*发送端需要处理的RTCP数据类型*/
        case BASE_RTCP_FB_NACK_INFO:
        case BASE_RTCP_RR_INFO:
        case BASE_RTCP_XR_REFERENCETIME_INFO:
        case BASE_RTCP_FB_TRANSPORT_INFO:
        case BASE_RTCP_SPEC_FB_PLI_INFO:
        case BASE_RTCP_SPEC_FB_FIR_INFO:
        case BASE_RTCP_SPEC_FB_REMB_INFO:
            return BASE_RTCPROLE_SEND;
            
        /*接收端需要处理的RTCP数据类型*/
        case BASE_RTCP_SR_INFO:
        case BASE_RTCP_XR_DLRR_INFO:
            return BASE_RTCPROLE_RECV;
            
        /*其他类型的不做处理*/
        default:
            /*调用RTCP通讯模块接口，获取其私有类型RTCP的信息*/
            bool    bSender;
            unsigned int  uiRetVal = RtcpComm_IsForSender(pscRtcpBuf, &bSender);
            if(0 == uiRetVal)
            {
                return (true == bSender)?BASE_RTCPROLE_SEND:BASE_RTCPROLE_RECV;
            }
            else
            {
				return BASE_RTCPROLE_UNKNOW;
            }
    }
}


BASE_RTCPROLE_EN Base_RtcpGetRole(unsigned char *pscRtcpBuf)
{
    /*获取当前的RTCP类型*/
    BASE_RTCP_TYPE enRtcpType = Base_RtcpGetType(pscRtcpBuf);
    return Base_RtcpGetRoleByType(enRtcpType, pscRtcpBuf);
}

/*是否为重传包*/
bool Base_RtpIsNack(unsigned char *pucRtpBuffer, unsigned int uiRtpSize)
{
    /******************************************************************
     0                   1                   2                   3
     0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    | Ver |R|  res  |            other              |  Padding Len  |
    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    Ver: 版本
    R  : 重传标记， 1表示重传包，0表示非重传包
    *******************************************************************/
    /*确认padding位是否为1*/
    if((pucRtpBuffer[0]&0x20) == 0x00)
    {
        return false;
    }

    /*HIK PADDING方案长度为4字节，过滤掉其他长度的padding*/
    if(pucRtpBuffer[uiRtpSize - 1] < BASE_RTP_MIN_PADLEN)
    {
        return false;
    }

    if((pucRtpBuffer[uiRtpSize - 4] & 0x10) == 0x10)
    {
        /*打了HIK重传标记，为重传包*/
        return true;
    }
    else
    {
        return false;
    }
} 



const char *Base_RtcpGetTypeStr(BASE_RTCP_TYPE enRtcpType)
{
    switch(enRtcpType)
    {
        case BASE_RTCP_INVAILED:                return "INVAILED";
        case BASE_RTCP_SR_INFO:                 return "SR";
        case BASE_RTCP_XR_DLRR_INFO:            return "XR_DLRR";
        case BASE_RTCP_FB_NACK_INFO:            return "NACK";
        case BASE_RTCP_RR_INFO:                 return "RR";
        case BASE_RTCP_XR_REFERENCETIME_INFO:   return "XR_REF";
        case BASE_RTCP_FB_TRANSPORT_INFO:       return "TCC";
        case BASE_RTCP_SPEC_FB_PLI_INFO:        return "PLI";
        case BASE_RTCP_SPEC_FB_FIR_INFO:        return "FIR";
        case BASE_RTCP_SPEC_FB_REMB_INFO:       return "REMB";
        case BASE_RTCP_SEDS_INFO:               return "SEDS";
        case BASE_RTCP_APP:                     return "App";
        default:                                return "UNKNOW";
    }
}

char *Base_RtcpMultiPrint(unsigned char *pucDataBuf, unsigned int uiDataLen, char *pscPrintBuf, unsigned int uiPrintBufLen)
{
    unsigned int uiNowRtcpOffSet = 0;
    unsigned char *pucNowRtcpBuffer;

    snprintf(pscPrintBuf, uiPrintBufLen, "MultiRtcp <ssrc:0x%x>: ", Base_RtcpGetSsrc(pucDataBuf));
    while(uiNowRtcpOffSet < uiDataLen)
    {
        pucNowRtcpBuffer = (unsigned char *)pucDataBuf + uiNowRtcpOffSet;
        snprintf(pscPrintBuf + strlen(pscPrintBuf), uiPrintBufLen - strlen(pscPrintBuf), "%s ", Base_RtcpGetTypeStr(Base_RtcpGetType(pucNowRtcpBuffer)));
        uiNowRtcpOffSet += (Base_RtcpGetLen(pucNowRtcpBuffer) + 4);
    }
    return pscPrintBuf;
}

char *Base_RtpInfoShow(unsigned char *pucRtpBuffer, unsigned int uiRtpBufferLen, char *pucRtpInfoBuf)
{
    // Version
    const unsigned char V = pucRtpBuffer[0] >> 6;
    // Padding
    const bool P = ((pucRtpBuffer[0] & 0x20) == 0) ? false : true;
    // eXtension
    const bool X = ((pucRtpBuffer[0] & 0x10) == 0) ? false : true;
    const unsigned char CC = pucRtpBuffer[0] & 0x0f;
    const bool M = ((pucRtpBuffer[1] & 0x80) == 0) ? false : true;
    
    const unsigned char PT = pucRtpBuffer[1] & 0x7f;
    
    const unsigned short SEQ =
        (pucRtpBuffer[2] << 8) + pucRtpBuffer[3];
    
    const unsigned char* ptr = &pucRtpBuffer[4];
    
    unsigned int TP = ntohl(*(unsigned int *)ptr);
    ptr += 4;
    
    unsigned int SSRC = ntohl(*(unsigned int *)ptr);
    ptr += 4;

    const unsigned int CSRCocts = CC * 4;

    ptr += CSRCocts * 4;
    unsigned int uiHeaderLength = 12 + CSRCocts;

    unsigned int XLen = 0;
    if (X) {
      uiHeaderLength += 4;
      ptr += 2;
    
      // in 32 bit words
      XLen = ntohs(*(unsigned short *)ptr);
      ptr += 2;
      XLen *= 4;  // in bytes

      uiHeaderLength += XLen;
    }

    const unsigned int uiPaddingLength = (P && uiHeaderLength < uiRtpBufferLen) ? pucRtpBuffer[uiRtpBufferLen-1] : 0;

    sprintf(pucRtpInfoBuf, "len:%u V:%u P:%u X:%u nCSRC:%u M:%u PT:%u SEQ:%d TP:%08x SSRC:%08x  HdrLen:%u  XLen:%u PLen:%u", 
                             uiRtpBufferLen, V, P, X, CC, M, PT, SEQ, TP, SSRC, uiHeaderLength, XLen, uiPaddingLength);
    return pucRtpInfoBuf;
}

void Base_RtcpStatistic(BASE_STATISTIC_RTCP_ST *pstRtcpInfo, unsigned char *pucDataBuf, unsigned int uiDataLen)
{
    unsigned int uiNowRtcpOffSet = 0;
    unsigned char *pucNowRtcpBuffer;
    while(uiNowRtcpOffSet < uiDataLen)
    {
        pucNowRtcpBuffer = (unsigned char *)pucDataBuf + uiNowRtcpOffSet;
        BASE_RTCP_TYPE enRtcpType = Base_RtcpGetType((unsigned char *)pucNowRtcpBuffer);

        pstRtcpInfo->uiSsrc = Base_RtcpGetSsrc((unsigned char *)pucNowRtcpBuffer);
        pstRtcpInfo->uiTotalCnt++;

        switch(enRtcpType)
        {        
            case BASE_RTCP_SR_INFO:  
                pstRtcpInfo->uiSRCnt++; 
                break;
            case BASE_RTCP_XR_DLRR_INFO:
                pstRtcpInfo->uiXrDlrrCnt++; 
                break;
            case BASE_RTCP_FB_NACK_INFO:
                pstRtcpInfo->uiNackCnt++; 
                break;
            case BASE_RTCP_RR_INFO:
                pstRtcpInfo->uiRRCnt++; 
                break;
            case BASE_RTCP_XR_REFERENCETIME_INFO:
                pstRtcpInfo->uiXrRefTimeCnt++; 
                break;
            case BASE_RTCP_FB_TRANSPORT_INFO:
                pstRtcpInfo->uiTCCCnt++; 
                break;
            case BASE_RTCP_SPEC_FB_PLI_INFO:
                pstRtcpInfo->uiPliCnt++; 
                break;
            case BASE_RTCP_SPEC_FB_FIR_INFO:
                pstRtcpInfo->uiFirCnt++; 
                break;
            case BASE_RTCP_SPEC_FB_REMB_INFO:
                pstRtcpInfo->uiRembCnt++; 
                break;
            case BASE_RTCP_SEDS_INFO:
                pstRtcpInfo->uiSedsCnt++; 
                break;
            case BASE_RTCP_APP:
                pstRtcpInfo->uiAppCnt++; 
                break;
            default:  
                pstRtcpInfo->uiUnknowCnt++;
                break;
        }
        uiNowRtcpOffSet += (Base_RtcpGetLen(pucNowRtcpBuffer) + 4);
    }
}


unsigned int Base_RtpParseHeader(const unsigned char *pucRtpBuffer, const unsigned int uiRtpLen, BASE_RTPHDR_ST* pstHeader)
{
  //const unsigned char* const pucRTPDataBegin = pucRtpBuffer;                        /*指向缓冲开始*/
  const unsigned char* const pucRTPDataEnd   = pucRtpBuffer + uiRtpLen;             /*指向缓冲结束*/

  const unsigned int length = uiRtpLen;                                           /*长度*/
  if (length < BASE_RTP_MINLEN || length > BASE_RTP_MAXLEN) {                  /*长度不可能小于最小长度, 也不能大于最大长度*/
    return 1;
  }

  // Version
  const unsigned char V = pucRtpBuffer[0] >> 6;                                     /*版本号*/
  // Padding
  const bool P = ((pucRtpBuffer[0] & 0x20) == 0) ? false : true;    /*Padding标记*/
  // eXtension
  const bool X = ((pucRtpBuffer[0] & 0x10) == 0) ? false : true;    /*扩展头标记*/
  const unsigned char CC = pucRtpBuffer[0] & 0x0f;                                  /*CC*/
  const bool M = ((pucRtpBuffer[1] & 0x80) == 0) ? false : true;    /*Mark位*/

  const unsigned char PT = pucRtpBuffer[1] & 0x7f;                                  /*Payload Type*/

  const unsigned short sequenceNumber =
      (pucRtpBuffer[2] << 8) + pucRtpBuffer[3];                                 /*Sequence Number*/

  const unsigned char* ptr = &pucRtpBuffer[4];                                      /*指向偏移4*/

  unsigned int RTPTimestamp = ntohl(*(unsigned int *)ptr);                      /*获取TimeStamp*/
  ptr += 4;                                                                     /*后移4*/

  unsigned int SSRC = ntohl(*(unsigned int *)ptr);                              /*SSRC*/
  ptr += 4;                                                                     /*后移4*/

  if (V != BASE_RTP_VER) {                                                      /*校验版本号*/
    return 2;                                                           /*错误的RTP包， 解析失败*/
  }

  const unsigned int CSRCocts = CC * 4;                                           /*CSRC占用空间*/

  pstHeader->bMarkerBit = M;                                                    /*赋值RTP头*/
  pstHeader->ucPayloadType = PT;                                                /*赋值RTP头*/
  pstHeader->usSequenceNumber = sequenceNumber;                                 /*赋值RTP头*/
  pstHeader->uiTimestamp = RTPTimestamp;                                        /*赋值RTP头*/
  pstHeader->uiSsrc = SSRC;                                                     /*赋值RTP头*/
  pstHeader->ucNumCSRCs = CC;                                                   /*赋值RTP头*/

  if ((ptr + CSRCocts) > pucRTPDataEnd) {                                       /*如果已经超过缓冲区结尾*/
    return 3;                                                           /*错误的RTP包， 解析失败*/
  }

  if (!P) {                                                                     /*没有Padding*/
    pstHeader->uiPaddingLength = 0;                                             /*Padding长度为0*/
  }

  for (unsigned char i = 0; i < CC; ++i) {
    unsigned int CSRC = ntohl(*(unsigned int *)ptr);                            /*获取CSRC*/
    ptr += 4;                                                                   /*后移4*/
    pstHeader->auiCSRCs[i] = CSRC;                                              /*赋值头*/
  }

  pstHeader->uiHeaderLength = 12 + CSRCocts;                                    /*当前已经解析的头长度*/

  if (X) {                                                                      /*如果有RTP扩展头*/
    /* RTP pstHeader extension, RFC 3550.
     0                   1                   2                   3
     0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    |      defined by profile       |           length              |
    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    |                      pstHeader extension                      |
    |                             ....                              |
    */
    const ptrdiff_t remain = pucRTPDataEnd - ptr;                               /*当前剩余数据大小*/
    if (remain < 4) {
      return 4;                                                         /*已经不足4， 说明这个RTP包有问题， 解析失败*/
    }

    pstHeader->uiHeaderLength += 4;                                             /*长度+4*/

    //unsigned short definedByProfile = ntohs(*(unsigned short *)ptr);                /*扩展头Profile ID*/
    ptr += 2;                                                                   /*后移2*/

    // in 32 bit words
    unsigned int XLen = ntohs(*(unsigned short *)ptr);                            /*整个扩展头长度*/
    ptr += 2;                                                                   /*后移2*/
    XLen *= 4;  // in bytes

    if (static_cast<unsigned int>(remain) < (4 + XLen)) {                         /*长度不足， 说明这个RTP包有问题， 解析失败*/
      return 5;
    }

    pstHeader->uiHeaderLength += XLen;                                          /*加上扩展头的长度*/
  }

  if (pstHeader->uiHeaderLength > static_cast<unsigned int>(length)) {            /*校验长度*/
    return 6;
  }

  if (P) {                                                                      /*有padding位*/
    // Packet has padding.
    if (pstHeader->uiHeaderLength != static_cast<unsigned int>(length)) {         /*除RTP头之外，有负载数据*/
      // Packet is not pstHeader only. We can parse padding length now.
      pstHeader->uiPaddingLength = *(pucRTPDataEnd - 1);                        /*取padding长度*/
    } else {
      // Packet is pstHeader only. We have no clue of the padding length.
      return 7;
    }
  }

  if (pstHeader->uiHeaderLength + pstHeader->uiPaddingLength >                  /*头大小+padding大小， 不可能超过整个RTP的数据长度*/
      static_cast<unsigned int>(length))
    return 8;

  return 0;
}

/** @fn     Base_RtpGetAllHeadSize
*   @brief  获取RTP所有的头长度，包括CSC以及扩展头
*   @param
*   @return
*/
int Base_RtpGetAllHeadSize(unsigned char *pucDataBuf, unsigned int uiDataLen)
{
	unsigned short usExtSize = 0;
	unsigned int   uiCSCCnt = 0;
	unsigned int   uiAllHeadSize = 0;

	uiCSCCnt = pucDataBuf[0] & 0xF;

	/*判断是否有拓展头部*/
	if (0x10 == (pucDataBuf[0] & 0x10))
	{
		/*跳过12字节头部 + CSC*/
		unsigned char *pscExternHeader = pucDataBuf + 12 + (uiCSCCnt * 4);
		/* 0x1000扩展(两字节扩展)，0xBEDE(一字节扩展) */
		if (ntohs(*(unsigned short *)(&pscExternHeader[0])) == 0x1000
			|| ntohs(*(unsigned short *)(&pscExternHeader[0])) == 0xBEDE)
		{
			/*获取扩展数据长度*/
			usExtSize = ntohs(*(unsigned short *)(&pscExternHeader[2])) * 4 + 4;
			if (usExtSize > 1500)
			{
				return 12 + (uiCSCCnt * 4);
			}
		}
		else
		{
			return 12 + (uiCSCCnt * 4);
		}
	}

	uiAllHeadSize = 12 + (uiCSCCnt * 4) + usExtSize;  /* 计算总的头长度 */
	if (uiAllHeadSize >= uiDataLen)
	{
		return 12;
	}

	return uiAllHeadSize;
}


/** @fn     Base_RtpBitstreamReversal
*   @brief  RTP包内实际码流进行两两翻转
*           将码流数据反转，对接第三方设备某些格式的音频流可能需要进行该操作
*   @param
*   @return 
*/
int Base_RtpBitstreamReversal(unsigned char *pucDataBuf, unsigned int uiDataLen)
{
    unsigned int uiAllHeadSize = Base_RtpGetAllHeadSize(pucDataBuf, uiDataLen);
    
    if(uiAllHeadSize >= uiDataLen)
    {
        return -1;
    }

    unsigned char *pucStream = pucDataBuf + uiAllHeadSize;                           /* 找的流开始位置 */
    unsigned int uiReversalLen = uiDataLen - uiAllHeadSize;                          /* 先去除头长度 */
    uiReversalLen = uiReversalLen - Base_RtpGetPaddingLen(pucDataBuf, uiDataLen);    /* 再去除padding长度 */
    uiReversalLen = uiReversalLen - uiReversalLen % 2;                               /* 保证码流长度为奇数时最后一位不反转 */
    
    /* 进行码流反转 */
    for (unsigned int i = 0; i < uiReversalLen; i += 2)
    {
        unsigned char ucTemp = pucStream[i];
        pucStream[i] = pucStream[i+1];
        pucStream[i+1] = ucTemp;
    }
    
    return 0;    
}

#define NETADAPT_NAL_STAP_A    24
#define NETADAPT_NAL_FU        28

#define NETADAPT_NAL_SPS_ID    7
#define NETADAPT_NAL_IDR_ID    5
#define NETADAPT_NAL_PPS_ID    8


#define NETADAPT_HEVC_NAL_FU   49

#define NETADAPT_HEVC_NAL_VPS  32
#define MAX_SAMPLE_FREQ_INDEX  13
/*判断该RTP数据包是否为I帧的包，要区分是H264还是H265进行判断*/
bool Base_RtpIsKeyFrame(unsigned char *pucRtpPacket, unsigned int uiDataLen, bool bVideoEncH265)
{

	unsigned int uiAllHeadSize = Base_RtpGetAllHeadSize(pucRtpPacket, uiDataLen);
	unsigned char *pucRealVideoPayload = pucRtpPacket + uiAllHeadSize;  /*偏移所有头长度则为实际视频负载*/

	unsigned char uiNaluType = 0;
	/*区分H265和H264,获取正确的NALTYPE*/
	if (true == bVideoEncH265)
	{
		/*h265，现在hik一体式终端，发送的h265码流，h265的描述信息为顺序为VPS,SPS,PPS*/
		/*
		NALType:6个比特，NALU类型。
		在判断帧类型中起重要作用。
		当其类型为FU(49)时，
		需要通过下一字节FU Header中的Type类型判断是否为关键帧。
		否则直接判断NALType的值，
		当该值为16,17,18,19,20,21,sps(33),pps(34)时，认为是关键帧。
		*/
		uiNaluType = (*(unsigned char *)pucRealVideoPayload >> 1) & 0x3f;

		/*
		h265码流， type值是49，说明其是FU-A包
		*/
		if (NETADAPT_HEVC_NAL_FU == uiNaluType)
		{
			unsigned char uiNaluType = ((unsigned char)pucRealVideoPayload[2]) & 0x3f;

			if ((19 == uiNaluType) ||
				(32 == uiNaluType) ||
				(33 == uiNaluType) ||
				(34 == uiNaluType) ||
				(39 == uiNaluType))
			{
				return true;
			}
		}
		else
		{
			unsigned char uiNaluType = (*(unsigned char *)pucRealVideoPayload >> 1) & 0x3f;
			if (NETADAPT_HEVC_NAL_VPS == uiNaluType)
			{
				return true;
			}
		}
	}
	else
	{
		/*
		获取当前NAL单元的类型，
		在判断帧类型中起重要作用。
		当类型为FU_A(28)时，
		需要通过下一字节FU Header中的Type类型判断是否为关键帧。
		否则直接根据该Type的值判断帧类型。
		当该值为Idr(5),sps(7),pps(8)时，认为是关键帧
		*/
		uiNaluType = ((*(unsigned char *)pucRealVideoPayload) & 0x1f);

		/*
		h264码流， type值是28，说明其是FU-A包
		*/
		if (NETADAPT_NAL_FU == uiNaluType)
		{
			unsigned char uiNaluType = ((unsigned char)pucRealVideoPayload[1]) & 0x1f;
			if (NETADAPT_NAL_IDR_ID == uiNaluType)
			{
				return true;
			}
		}
		else
		{
			unsigned char uiNaluType = ((*(unsigned char *)pucRealVideoPayload) & 0x1f);
			if (uiNaluType == NETADAPT_NAL_STAP_A)
			{
				uiNaluType = ((unsigned char)pucRealVideoPayload[3]) & 0x1f;
			}
			if ((NETADAPT_NAL_SPS_ID == uiNaluType) ||
				(NETADAPT_NAL_PPS_ID == uiNaluType) ||
				(NETADAPT_NAL_IDR_ID == uiNaluType))

			{
				return true;
			}
		}

	}
	return  false;
}

#include "H264ParseSPS.h"
/* 从视频码流中获取视频分辨率信息,要区分是H264还是H265进行判断*/
int Base_RtpGetVideoResolution(unsigned int* uiWidth, unsigned int* uiHeight, unsigned char *pucRtpPacket, unsigned int uiDataLen, bool bVideoEncH265)
{

	unsigned int uiAllHeadSize = Base_RtpGetAllHeadSize(pucRtpPacket, uiDataLen);
	unsigned int uiPaddingLen = Base_RtpGetPaddingLen(pucRtpPacket, uiDataLen);
	unsigned int uiPayloadLen = uiDataLen - uiAllHeadSize - uiPaddingLen; /* 实际负载长度 */
	unsigned char *pucRealVideoPayload = pucRtpPacket + uiAllHeadSize;  /*偏移所有头长度则为实际视频负载*/

	
	unsigned char uiNaluType = 0;
	/*区分H265和H264,获取正确的NALTYPE*/
	if (true == bVideoEncH265)
	{
		return -1; // h265格式目前还不支持
	}
	else
	{
		uiNaluType = ((*(unsigned char *)pucRealVideoPayload) & 0x1f);

		if (NETADAPT_NAL_SPS_ID == uiNaluType)
		{
			sps_info_struct spsInfo;
			if (h264_parse_sps(pucRealVideoPayload, uiPayloadLen, &spsInfo))
			{
				*uiWidth = spsInfo.width;
				*uiHeight = spsInfo.height;
				return 0;
			}
		}
	}
	return  -1;
}
