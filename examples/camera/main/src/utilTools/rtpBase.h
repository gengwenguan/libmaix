/**************************************************************************************
*  Copyright(C),Your Company 
*  Filename:    rtpbase.h
*  Description: 对rtp和rtcp数据进行操作的基础接口
*  Author:      gengwenguan
*  Create:      2024-01-24
*  Modification history:
**************************************************************************************/
#ifndef __RTPBASE_H__
#define __RTPBASE_H__

/****************
  0                   1                   2                   3
  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 |V=2|P| RC/FMT  |       PT      |             length            |
 +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 |                              SSRC                             |
 +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

 ------RFC5450------
 PT = 195 :  IJ    --- 扩展Jitter报告
 ------RFC3550------
 PT = 200 :  SR    --- 发送端报告
 PT = 201 :  RR    --- 接收端报告
 PT = 202 :  SDES  --- 源端描述
 PT = 203 :  BYE   --- 离开会话
 PT = 204 :  APP   --- 特定于应用
 ------RFC4585------
 PT = 205 :  RTPFB --- 传输层反馈
             FMT = 1    NACK
             FMT = 3    TMMBB
             FMT = 4    TMMBN
 ------RFC5104------
 PT = 206 :  PSFB  --- 负载相关反馈
             FMT = 1    PLI
             FMT = 2    SLI
             FMT = 4    FIR
             REMB= 15   REMB
 ------RFC3611------
 PT = 207 :  XR
              BT = 4     REFERENCETIME
              BT = 5     DLRR
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|V=2|P|reserved |   PT=XR=207   |             length            |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                              SSRC                             |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
:                         report blocks                         :
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

 0                     1                     2                     3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|       BT        | type-specific |          block length            |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
:              type-specific block contents                        :
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
            
****************/
#define BASE_RTCP_PT_SR                 200
#define BASE_RTCP_PT_RR                 201
#define BASE_RTCP_PT_SEDS               202
#define BASE_RTCP_PT_BYE                203
#define BASE_RTCP_PT_APP                204
#define BASE_RTCP_PT_RTPFB              205
#define BASE_RTCP_PT_PSFB               206
#define BASE_RTCP_PT_XR                 207

#define BASE_RTCP_PT_RTPFB_SUBPT_NACK   1
#define BASE_RTCP_PT_RTPFB_SUBPT_TMMBB  3
#define BASE_RTCP_PT_RTPFB_SUBPT_TMMBN  4
#define BASE_RTCP_PT_RTPFB_SUBPT_TCC    15

#define BASE_RTCP_PT_PSFB_SUBPT_PLI     1
#define BASE_RTCP_PT_PSFB_SUBPT_SLI     2
#define BASE_RTCP_PT_PSFB_SUBPT_FIR     4
#define BASE_RTCP_PT_PSFB_SUBPT_REMB    15

#define BASE_RTCP_PT_XR_BT_DLRR         5  /* 协议标准此处为5 */
#define BASE_RTCP_PT_XR_BT_REFTIME      4  /* 协议标准此处为4 */

#define BASE_RTP_VER                    2
#define BASE_RTP_MINLEN                 12
#define BASE_RTP_MAXLEN                 1472    /* MTU(1500B) - IP头(20B) -UDP头（8B）= 1472Bytes */  

#define BASE_RTP_CSRCSIZE               15  // RFC 3550 page 13
#define BASE_RTP_MIN_PADLEN             4   /*padding位长度*/

#define BASE_RTP_MIN_PT                 0    /*最小PT大小*/
#define BASE_RTP_MAX_PT                 127  /*最大PT大小*/
#define BASE_RTP_INVALID_PT             0xff /*无效PT值*/

inline bool BaseRtpCheckPt(unsigned char u8Pt)
{
    return u8Pt <= BASE_RTP_MAX_PT ? true : false;
}


typedef enum
{    
    BASE_RTCP_INVAILED,
    BASE_RTCP_SR_INFO,
    BASE_RTCP_XR_DLRR_INFO,
    BASE_RTCP_FB_NACK_INFO,
    BASE_RTCP_RR_INFO,
    BASE_RTCP_XR_REFERENCETIME_INFO,
    BASE_RTCP_FB_TRANSPORT_INFO,
    BASE_RTCP_SPEC_FB_PLI_INFO,
    BASE_RTCP_SPEC_FB_FIR_INFO,
    BASE_RTCP_SPEC_FB_REMB_INFO,
    BASE_RTCP_SEDS_INFO,
    BASE_RTCP_APP,       /*RTCP APP类型的消息，具体需要RTCP COMM模块解析*/
}BASE_RTCP_TYPE;

/*RTCP类型,发送端,接收端*/
enum BASE_RTCPROLE_EN
{
    BASE_RTCPROLE_RECV   = 1,
    BASE_RTCPROLE_SEND   = 2,
    BASE_RTCPROLE_UNKNOW = 3,
};

typedef struct
{
    unsigned int   uiTotalCnt;
	unsigned int   uiSsrc;
	unsigned int   uiSRCnt;
	unsigned int   uiRRCnt;
	unsigned int   uiXrRefTimeCnt;
	unsigned int   uiXrDlrrCnt;
	unsigned int   uiNackCnt;
	unsigned int   uiTCCCnt;
	unsigned int   uiRembCnt;
	unsigned int   uiPliCnt;
	unsigned int   uiFirCnt;
	unsigned int   uiSedsCnt;
	unsigned int   uiUnknowCnt;
	unsigned int   uiAppCnt;
}BASE_STATISTIC_RTCP_ST;

typedef struct
{
  bool           bMarkerBit;
  unsigned char  ucPayloadType;
  unsigned short usSequenceNumber;
  unsigned int   uiTimestamp;
  unsigned int   uiSsrc;
  unsigned char  ucNumCSRCs;
  unsigned int   auiCSRCs[BASE_RTP_CSRCSIZE];
  unsigned int   uiPaddingLength;
  unsigned int   uiHeaderLength;
}BASE_RTPHDR_ST;


typedef enum
{
	BASE_RTP,
	BASE_RTCP,
	BASE_UNKNOW,
}BASE_DATA_TYPE;

/**
* Base_TimeNowUs get now time  //使用对应平台的系统接口获取精准的UTC至今的微妙数
* @return number of microsecond since January 1, 1970.
* @sa
*/
unsigned long long Base_TimeNowUs();
/**
* Base_TimeNow get now time //使用c++标准时间接口获取的时间可能不精准
* @return number of microsecond since January 1, 1970.
* @sa
*/
unsigned long long Base_TimeNow();
/**
* Base_GetTimeTickMs get now time
* @return number of milliseconds since system start. cpu time.
* @sa HPR_TimeNow
*/
unsigned int Base_GetTimeTickMs();
/*获取数据的具体类型，是RTP还是RTCP*/
BASE_DATA_TYPE Base_GetDataType(unsigned char *pucBuffer, unsigned int uiDataLen);
/*获取RTP或者RTCP中的ssrc*/
unsigned int Base_DataGetSsrc(unsigned char *pucBuffer, unsigned int uiDataLen);
/*获取时间差*/
unsigned int Base_GetTimerPeriod(unsigned int uiStartMs, unsigned int uiStopMs);
/*获取RTCP包的PT值*/
unsigned char  Base_RtcpGetPt(unsigned char *pucRtcpBuffer);
/*获取RTCP包的SUB PT值*/
unsigned char  Base_RtcpGetSubPt(unsigned char *pucRtcpBuffer);
/*获取RTCP的SSRC值*/
unsigned int Base_RtcpGetSsrc(unsigned char *pucRtcpBuffer);
/*配置RTCP的SSRC值*/
void Base_RtcpSetSsrc(unsigned char *pucRtcpBuffer,  unsigned int uiDataLen, unsigned int uiSsrc);
/*设置RTCP的SSRC值*/
void Base_RtpSetSsrc(unsigned char *pucRtpBuffer, unsigned int uiSsrc);
/*获取RTP Seq*/
unsigned short Base_RtpGetSeq(unsigned char *pucRtpBuffer);
/*配置RTP Seq*/
void Base_RtpSetSeq(unsigned char *pucRtpBuffer, unsigned short sequence);
bool Base_RtpIsNewerSeq(unsigned short value, unsigned short prev_value);
/*获取RTP Ver*/
unsigned char Base_RtpGetVer(unsigned char *pucRtpBuffer);
/*获取RTP包头中的Mark值*/
bool Base_RtpGetMark(unsigned char *pucRtpBuffer);
/*配置RTP包头中的Mark值*/
void Base_RtpSetMark(unsigned char *pucRtpBuffer, bool bMark);
/*获取RTP包头中的Extension值*/
bool Base_RtpGetExtension(unsigned char *pucRtpBuffer);
/*配置RTP包头中的Extension值*/
void Base_RtpSetExtension(unsigned char *pucRtpBuffer, bool bExtension);
/*获取RTP包头中的Padding值*/
bool Base_RtpGetPadding(unsigned char *pucRtpBuffer);
/*配置RTP包头中的Padding值*/
void Base_RtpSetPadding(unsigned char *pucRtpBuffer, bool bPadding);
/*获取RTP包头中的Padding长度*/
unsigned int Base_RtpGetPaddingLen(unsigned char *pucRtpBuffer, unsigned int uiRtpLen);
/*删除RTP包头中的Padding位并且去除实际负载中的padding,会修改数据包的长度*/
void Base_RtpRmPadding(unsigned char *pucRtpBuffer, unsigned int& uiRtpLen);
/*获取RTP PT*/
unsigned char Base_RtpGetPt(unsigned char *pucRtpBuffer);
/*配置RTP PT*/
void   Base_RtpSetPt(unsigned char *pucRtpBuffer, unsigned char ucPayloadType);
/*获取Rtcp子类型*/
unsigned char Base_RtcpGetSubBt(unsigned char *pucRtcpBuffer);
/*获取RTCP包长度*/
unsigned short Base_RtcpGetLen(unsigned char *pucRtcpBuffer);
/*获取RTP TimeStamp*/
unsigned int Base_RtpGetTimestamp(unsigned char *pucRtpBuffer);
/*设置RTP TimeStamp*/
void Base_RtpSetTimestamp(unsigned char *pucRtpBuffer, unsigned int uiTimestamp);
/*判断RTP 是否为新的时间戳*/
bool Base_RtpIsNewTimestamp(unsigned int value, unsigned int prev_value);
/*获取RTP SSRC*/
unsigned int Base_RtpGetSsrc(unsigned char *pucRtpBuffer);
/*是否为重传包*/
bool Base_RtpIsNack(unsigned char *pucRtpBuffer, unsigned int uiRtpSize);
/*获取RTCP类型*/
BASE_RTCP_TYPE Base_RtcpGetType(unsigned char *pucRtcpBuffer);
/*获取字符串类型的RTCP类型*/
const char* Base_RtcpGetStrType(unsigned char *pucRtcpBuffer);
/*获取RTCP复合包的全部信息*/
char *Base_RtcpMultiPrint(unsigned char *pucDataBuf, unsigned int uiDataLen, char *pscPrintBuf, unsigned int uiPrintBufLen);
/*显示RTP信息*/
char *Base_RtpInfoShow(unsigned char *pucRtpBuffer, unsigned int uiRtpBufferLen, char *pucRtpInfoBuf);
/*获取RTCP为发送端，还是接收端数据*/
BASE_RTCPROLE_EN Base_RtcpGetRoleByType(BASE_RTCP_TYPE enRtcpType, unsigned char *pscRtcpBuf);
/*获取RTCP为发送端，还是接收端数据*/
BASE_RTCPROLE_EN Base_RtcpGetRole(unsigned char *pscRtcpBuf);
/*统计RTCP数据*/
void Base_RtcpStatistic(BASE_STATISTIC_RTCP_ST *pstRtcpInfo, unsigned char *pucDataBuf, unsigned int uiDataLen);
unsigned int Base_RtpParseHeader(const unsigned char *pucRtpBuffer, const unsigned int uiRtpLen, BASE_RTPHDR_ST* pstHeader);
/*获取RTP所有的头长度，包括CSC以及扩展头*/
int Base_RtpGetAllHeadSize(unsigned char *pucDataBuf, unsigned int uiDataLen);
/* RTP包内实际码流进行两两翻转 */
int Base_RtpBitstreamReversal(unsigned char *pucDataBuf, unsigned int uiDataLen);

/*判断该RTP数据包是否为I帧的包，要区分是H264还是H265进行判断*/
bool Base_RtpIsKeyFrame(unsigned char *pucRtpPacket, unsigned int uiDataLen, bool bVideoEncH265);

/* 从视频码流中获取视频分辨率信息,要区分是H264还是H265进行判断,return 0获取成功*/
int Base_RtpGetVideoResolution(unsigned int* uiWidth, unsigned int* uiHeight, unsigned char *pucRtpPacket, unsigned int uiDataLen, bool bVideoEncH265);


#endif
