/*********************************************************************************
  *Copyright(C),Your Company
  *FileName:  recorder.h
  *Author:    gengwenguan
  *Date:      2026-05-23
  *Description:  常驻滚动录像（Rolling Recorder）。
  *
  *  设计背景：
  *    监控系统不应该让用户手动 start/stop，进程启动即录、过期自动清理。
  *    单文件录到几个小时浏览器拖拽就崩了，因此采用"分片滚动"：
  *      - 每 10 分钟一片，单文件 ~30~80MB
  *      - 切片必须在 IDR 边界（fmp4Muxer 已经保证每个 fragment 都是 IDR 起头）
  *      - 文件命名 record/YYYYMMDD/YYYYMMDD_HHMMSS.mp4，按天分目录便于扫描和清理
  *
  *  滚动切片策略：
  *    OnLiveFragment 入口检查 (now - m_segStartMs >= 10min)；
  *    超过则 close 旧文件 → 新建按"现在 wallclock 对齐到 10min 整数边界"的下一片 →
  *    先写 init segment → 再写本次 fragment（即新片的首个 IDR 起头 fragment）。
  *
  *  init segment 的获取：
  *    LiveHub 缓存 init segment 并在 Subscribe 时立即回推；
  *    本类在 OnLiveInitSegment 中把它存到 m_initSeg 备用，切片时重写文件头。
  *    （注意 LiveHub 在 muxer 重启场景下也可能重发 init，我们用 assign 覆盖即可。）
  *
  *  线程安全：
  *    LiveHub 持锁回调 listener；本类 m_mu 保护文件 + init 缓存的并发。
**********************************************************************************/
#pragma once
#include <atomic>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "liveHub.h"

class C_RollingRecorder : public C_LiveHub::C_Listener
{
public:
    C_RollingRecorder();
    ~C_RollingRecorder() override;

    // 启动滚动录像。rootDir 是录像根目录，子目录会按天自动建。
    // 返回 0 成功；多次 Start 只第一次生效。
    // 单片时长不再由构造参数固化，而是 OnLiveFragment 每次按需读
    // C_AppConfig::GetSnapshot().record_segment_s，因此 web 改完即时生效，
    // 不需要 setter / atomic / 订阅回调。
    int  Start(const std::string& rootDir);

    // 停止录像（幂等）。析构时会自动 Stop。
    void Stop();

    bool        IsRecording() const;
    std::string CurrentFile() const;
    uint64_t    CurrentBytes() const { return m_curBytes.load(); }
    std::string RootDir()     const { return m_rootDir; }

    // C_LiveHub::C_Listener
    void OnLiveInitSegment(const uint8_t* data, size_t len) override;
    void OnLiveFragment   (const uint8_t* data, size_t len) override;

private:
    // 在持锁状态下：关闭当前文件，按 wallclock 创建下一片，写 init segment
    bool RollSegment_locked();

    // 持锁状态下：扫描 fragment buffer，把每个 traf 内的 tfdt(base_media_decode_time)
    // 减去本片首次见到该 track_ID 时的基准值，从而让每个 mp4 文件时间从 0 开始。
    // 这是修复"浏览器把分片的 start_time 当成 25578s 导致回放定位失败"的关键：
    // 直播链路上 PTS 是从进程启动累积的，但每个独立 mp4 文件应当自零起算。
    //
    // outFirstVideoTfdt：写盘前 tfdt 已经改写为相对时间，回填给本 fragment 的视频
    //   track tfdt（90kHz 时基），用于伴生 .idx 索引；多 traf 时只取首个 video。
    //   传 nullptr 表示不需要。
    void RewriteTfdtInPlace_locked(uint8_t* buf, size_t len,
                                   uint64_t* outFirstVideoTfdt);

    // 关闭当前 .idx 索引文件（写入 JSON 尾、close）。多次调用幂等。
    void CloseIdxFile_locked();
    // 打开新片对应的 .idx 文件并写入头（"frags":[ 前置部分）。
    bool OpenIdxFile_locked(const std::string& idxFullPath);
    // 向 .idx 追加一条 fragment 记录：[tfdt, byteOffset, byteSize]
    void AppendIdxEntry_locked(uint64_t tfdt90k,
                               uint64_t byteOffset,
                               uint32_t byteSize);

    // 拼当前应该用的文件路径：<root>/<YYYYMMDD>/<YYYYMMDD_HHMMSS>.mp4
    static std::string MakeSegmentPath(const std::string& root,
                                       std::string& dayDirOut,
                                       std::string& fileNameOut);

    // mkdir -p（缺什么建什么）
    static bool MkdirP(const std::string& path);

    // 单调时钟毫秒
    static int64_t NowMonoMs();

private:
    std::string          m_rootDir;

    mutable std::mutex   m_mu;
    std::vector<uint8_t> m_initSeg;         // 当前 init segment 缓存（用于切片时重写）
    std::ofstream        m_ofs;
    std::string          m_curFile;
    int64_t              m_segStartMs = 0;  // 当前片的开始时间（单调时钟）

    // 伴生索引文件 (<file>.mp4.idx)：精准 seek 用。
    //   v=1, ts=90000, init=<initSegBytes>, frags=[[tfdt90k, byteOffset, byteSize], ...]
    //   写法：增量 append，每来一个 fragment 写一条；切片或停止时写尾部并 close。
    std::ofstream        m_idxOfs;
    bool                 m_idxFirstFrag = true;  // 控制 JSON 数组逗号

    std::atomic<bool>    m_running{false};
    std::atomic<uint64_t> m_curBytes{0};

    // 每个文件独立的 tfdt 基准：first_seen[track_ID] = 本片第一次看到的 base_media_decode_time
    // 切片时清空；后续每个 fragment 写盘前用 (raw - base) 改写到位。
    std::unordered_map<uint32_t, uint64_t> m_tfdtBase;
};
