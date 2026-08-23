/*********************************************************************************
  *Copyright(C),Your Company
  *FileName:  recorder.cpp
  *Author:    gengwenguan
  *Date:      2026-05-23
  *Description:  常驻滚动录像（Rolling Recorder）实现，详见 recorder.h
**********************************************************************************/
#include "recorder.h"
#include "appConfig.h"
#include "logAdapt.h"

#include <chrono>
#include <ctime>
#include <sys/stat.h>
#include <sys/types.h>
#include <cstring>
#include <cerrno>

namespace {
// ---- 简易 ISO BMFF box 工具：仅用来读 size/type 与原地改写 tfdt ----
// 大端读写
static inline uint32_t Rd32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}
static inline uint64_t Rd64(const uint8_t* p) {
    return ((uint64_t)Rd32(p) << 32) | (uint64_t)Rd32(p + 4);
}
static inline void Wr32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static inline void Wr64(uint8_t* p, uint64_t v) {
    Wr32(p, (uint32_t)(v >> 32));
    Wr32(p + 4, (uint32_t)v);
}
} // namespace

C_RollingRecorder::C_RollingRecorder() = default;

C_RollingRecorder::~C_RollingRecorder()
{
    Stop();
}

int64_t C_RollingRecorder::NowMonoMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

bool C_RollingRecorder::MkdirP(const std::string& path)
{
    if (path.empty()) return false;
    // 逐级创建：从第一个 '/' 之后开始扫
    std::string acc;
    acc.reserve(path.size());
    for (size_t i = 0; i < path.size(); ++i) {
        char c = path[i];
        acc.push_back(c);
        if (c == '/' || i + 1 == path.size()) {
            if (acc == "/" || acc.empty()) continue;
            struct stat st{};
            if (stat(acc.c_str(), &st) == 0) {
                if (!S_ISDIR(st.st_mode)) return false;
                continue;
            }
            if (mkdir(acc.c_str(), 0755) != 0 && errno != EEXIST) {
                CLOG_ERR("mkdir(%s) failed: %s\n", acc.c_str(), strerror(errno));
                return false;
            }
        }
    }
    return true;
}

std::string C_RollingRecorder::MakeSegmentPath(const std::string& root,
                                               std::string& dayDirOut,
                                               std::string& fileNameOut)
{
    // 用 wallclock 命名（YYYYMMDD_HHMMSS），便于人眼识别和按时间排序
    // 开发板上 /etc/localtime 可能没配，直接 localtime_r 拿到的是 UTC，
    // 因此参考 logAdapt.cpp 的做法：先给 time_t 加 8h（UTC+8），再 localtime_r
    static constexpr int kChinaTimeOffsetSec = 8 * 60 * 60;
    std::time_t t = std::time(nullptr) + kChinaTimeOffsetSec;
    std::tm tmv{};
    localtime_r(&t, &tmv);

    char day[16]   = {0};
    char stamp[24] = {0};
    std::strftime(day,   sizeof(day),   "%Y%m%d",        &tmv);
    std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &tmv);

    dayDirOut  = std::string(day);
    fileNameOut = std::string(stamp) + ".mp4";

    std::string r = root;
    if (!r.empty() && r.back() != '/') r.push_back('/');
    return r + dayDirOut + "/" + fileNameOut;
}

int C_RollingRecorder::Start(const std::string& rootDir)
{
    if (m_running.load()) {
        CLOG_INF("RollingRecorder already started\n");
        return 0;
    }
    m_rootDir = rootDir;
    if (!MkdirP(m_rootDir)) {
        CLOG_ERR("RollingRecorder mkdir root(%s) failed\n", m_rootDir.c_str());
        return -1;
    }
    // 暂不打开文件，等首个 OnLiveInitSegment 或首个 OnLiveFragment 到来再切第一片
    // 这样能保证文件首段一定是 init segment，避免 init 还没到就开始写
    m_running.store(true);
    C_LiveHub::Inst().Subscribe(this);
    CLOG_INF("RollingRecorder started, root=%s (segment_s read from AppConfig per fragment)\n",
             m_rootDir.c_str());
    return 0;
}

void C_RollingRecorder::Stop()
{
    if (!m_running.exchange(false)) return;
    C_LiveHub::Inst().Unsubscribe(this);

    std::lock_guard<std::mutex> lk(m_mu);
    if (m_ofs.is_open()) {
        m_ofs.flush();
        m_ofs.close();
    }
    CloseIdxFile_locked();
    CLOG_INF("RollingRecorder stopped, last=%s, bytes=%llu\n",
             m_curFile.c_str(), (unsigned long long)m_curBytes.load());
}

std::string C_RollingRecorder::CurrentFile() const
{
    std::lock_guard<std::mutex> lk(m_mu);
    return m_curFile;
}

bool C_RollingRecorder::RollSegment_locked()
{
    // 1) 关闭旧文件 + 旧 .idx（先 idx 后 mp4，便于断电时尾部 ] 已落盘）
    CloseIdxFile_locked();
    if (m_ofs.is_open()) {
        m_ofs.flush();
        m_ofs.close();
        CLOG_INF("RollingRecorder rolled: close %s (%llu bytes)\n",
                 m_curFile.c_str(), (unsigned long long)m_curBytes.load());
    }

    // 2) 算新路径并 mkdir 当天目录
    std::string dayDir, fileName;
    std::string fullPath = MakeSegmentPath(m_rootDir, dayDir, fileName);
    std::string dayFull  = m_rootDir + (m_rootDir.empty() || m_rootDir.back()=='/' ? "" : "/") + dayDir;
    if (!MkdirP(dayFull)) {
        CLOG_ERR("RollingRecorder mkdir(%s) failed\n", dayFull.c_str());
        return false;
    }

    // 3) 打开新文件
    m_ofs.open(fullPath, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!m_ofs.is_open()) {
        CLOG_ERR("RollingRecorder open(%s) failed: %s\n", fullPath.c_str(), strerror(errno));
        return false;
    }
    m_curFile = fullPath;
    m_curBytes.store(0);
    m_segStartMs = NowMonoMs();

    // 每片独立从 0 起算：清空 tfdt 基准缓存
    m_tfdtBase.clear();

    // 4) 写 init segment（必须）。如果还没缓存到 init segment 说明 LiveHub 还没缓
    //    存（极少出现的极早启动场景），直接 close 等下次。
    if (m_initSeg.empty()) {
        CLOG_ERR("RollingRecorder roll: no init segment cached, abort segment\n");
        m_ofs.close();
        m_curFile.clear();
        return false;
    }
    m_ofs.write(reinterpret_cast<const char*>(m_initSeg.data()),
                (std::streamsize)m_initSeg.size());
    if (!m_ofs.good()) {
        CLOG_ERR("RollingRecorder write init segment failed\n");
        return false;
    }
    m_curBytes.fetch_add(m_initSeg.size());

    // 5) 打开伴生索引文件 <fullPath>.idx 并写头
    //    增量 append，每个 fragment 落盘后 fsync。客户端只在 GET .idx 时读，
    //    哪怕本片还没录完也能拿到当前已知的 frags（少几秒末尾，可接受）。
    //    若开 idx 失败不致命：没有 idx 时回放页会回退到全量流式。
    std::string idxPath = fullPath + ".idx";
    if (!OpenIdxFile_locked(idxPath)) {
        CLOG_ERR("RollingRecorder open idx(%s) failed\n", idxPath.c_str());
    }

    CLOG_INF("RollingRecorder rolled: open %s (init=%zu bytes)\n",
             fullPath.c_str(), m_initSeg.size());
    return true;
}

void C_RollingRecorder::OnLiveInitSegment(const uint8_t* data, size_t len)
{
    if (!data || len == 0) return;
    std::lock_guard<std::mutex> lk(m_mu);
    // 不管在不在录都缓存：进程启动顺序是 Subscribe 时回推 init，
    // 此时 m_running 已经是 true，但旧片可能还没切出来，照样要存
    m_initSeg.assign(data, data + len);
    CLOG_INF("RollingRecorder cache init segment: %zu bytes\n", len);
}

bool C_RollingRecorder::IsRecording() const
{
    std::lock_guard<std::mutex> lk(m_mu);
    return m_running.load() && m_ofs.is_open();
}

void C_RollingRecorder::OnLiveFragment(const uint8_t* data, size_t len)
{
    if (!m_running.load() || !data || len == 0) return;
    std::lock_guard<std::mutex> lk(m_mu);
    if (!m_running.load()) return;

    C_AppConfig::Snapshot config = C_AppConfig::GetInst().GetSnapshot();
    if (!config.record_enabled) {
        if (m_ofs.is_open()) {
            m_ofs.flush();
            m_ofs.close();
            CloseIdxFile_locked();
        }
        return;
    }
    int64_t now = NowMonoMs();
    // 每个 fragment 入口拉一次配置（mutex+memcpy 开销 < 1us，远低于 fragment 周期）：
    // 这样避免在类内维护副本和 setter，web 改完即时生效。
    // ClampSnapshot 已保证 record_segment_s ∈ [10, 3600]，这里无须再校验。
    int segmentMs = config.record_segment_s * 1000;
    bool needRoll = !m_ofs.is_open()
                  || (now - m_segStartMs) >= segmentMs;

    if (needRoll) {
        // fmp4Muxer 保证每个 fragment 都是从 IDR 起头，
        // 因此在 fragment 边界切片是安全的：新文件首段是 init+IDR-fragment
        if (!RollSegment_locked()) {
            // 切片失败时：旧文件已经被关掉，新文件没开成。下一帧再试。
            return;
        }
    }

    // LiveHub 把同一份 data 广播给多个订阅者（HLS 直播 + Recorder），
    // 不能 const_cast 直接改原 buffer 否则会污染直播链路；先拷再改。
    std::vector<uint8_t> tmp(data, data + len);
    uint64_t fragVideoTfdt = 0;
    bool     fragHasTfdt   = false;
    {
        // 调一次 Rewrite 顺便把视频 track tfdt 抓出来给 .idx 用
        uint64_t got = 0;
        RewriteTfdtInPlace_locked(tmp.data(), tmp.size(), &got);
        // RewriteTfdt 内部 m_tfdtBase 是 first-seen baseline，所以本 fragment 的
        // "fixed" 值（已经被改写到位的）就是 got；首个 fragment 时 got=0 也合法。
        fragVideoTfdt = got;
        fragHasTfdt   = true;
    }

    uint64_t fragOffset = m_curBytes.load();
    m_ofs.write(reinterpret_cast<const char*>(tmp.data()), (std::streamsize)tmp.size());
    if (!m_ofs.good()) {
        CLOG_ERR("RollingRecorder write fragment failed: %s\n", strerror(errno));
        return;
    }
    m_curBytes.fetch_add(tmp.size());

    // 同步写一条 .idx 记录（[tfdt90k, byteOffset, byteSize]）
    if (fragHasTfdt && m_idxOfs.is_open()) {
        AppendIdxEntry_locked(fragVideoTfdt, fragOffset, (uint32_t)tmp.size());
    }
}

// ---- tfdt 改写 ----
// 输入是一段 fragment 字节流，可能含一个或多个 top-level box（典型 fmp4 一个 fragment
// 是 [moof][mdat]），我们只关心 moof。结构：
//   moof
//     mfhd  (skip)
//     traf
//       tfhd  → 读出 track_ID
//       tfdt  → 改 base_media_decode_time
//       trun  (skip)
//     traf  (可能多个：video/audio)
//       ...
//
// box header：size(4 BE) + type(4)；当 size==1 时后跟 8 字节 largesize；size==0 表示
// 一直到文件末尾（这里不会出现）。fullbox 头还多 4 字节 version+flags。
void C_RollingRecorder::RewriteTfdtInPlace_locked(uint8_t* buf, size_t len,
                                                  uint64_t* outFirstVideoTfdt)
{
    if (outFirstVideoTfdt) *outFirstVideoTfdt = 0;
    auto fourcc = [](const uint8_t* p) {
        return (uint32_t(p[0])<<24)|(uint32_t(p[1])<<16)|(uint32_t(p[2])<<8)|uint32_t(p[3]);
    };
    constexpr uint32_t kMoof = 0x6d6f6f66; // 'moof'
    constexpr uint32_t kTraf = 0x74726166; // 'traf'
    constexpr uint32_t kTfhd = 0x74666864; // 'tfhd'
    constexpr uint32_t kTfdt = 0x74666474; // 'tfdt'
    // 视频在 fmp4Muxer 里是 avformat_new_stream 的第一个流，
    // ffmpeg mov muxer 写入 fmp4 时 track_ID = stream_index + 1，因此视频 = 1。
    constexpr uint32_t kVideoTrackId = 1;
    bool firstVideoTfdtCaptured = false;

    size_t pos = 0;
    while (pos + 8 <= len) {
        uint64_t bsize = Rd32(buf + pos);
        uint32_t btype = fourcc(buf + pos + 4);
        size_t   hdr   = 8;
        if (bsize == 1) {
            if (pos + 16 > len) break;
            bsize = Rd64(buf + pos + 8);
            hdr   = 16;
        }
        if (bsize < hdr || pos + bsize > len) break;

        if (btype == kMoof) {
            // 进入 moof：扫描内部 traf
            size_t mpos = pos + hdr;
            size_t mend = pos + (size_t)bsize;
            while (mpos + 8 <= mend) {
                uint64_t cs = Rd32(buf + mpos);
                uint32_t ct = fourcc(buf + mpos + 4);
                size_t   ch = 8;
                if (cs == 1) {
                    if (mpos + 16 > mend) break;
                    cs = Rd64(buf + mpos + 8);
                    ch = 16;
                }
                if (cs < ch || mpos + cs > mend) break;

                if (ct == kTraf) {
                    // 进入 traf：先扫一遍找 tfhd 拿 track_ID，再扫 tfdt 改写
                    uint32_t trackId = 0;
                    size_t tpos = mpos + ch;
                    size_t tend = mpos + (size_t)cs;

                    // pass 1：找 tfhd
                    {
                        size_t p = tpos;
                        while (p + 8 <= tend) {
                            uint64_t es = Rd32(buf + p);
                            uint32_t et = fourcc(buf + p + 4);
                            size_t   eh = 8;
                            if (es == 1) {
                                if (p + 16 > tend) break;
                                es = Rd64(buf + p + 8);
                                eh = 16;
                            }
                            if (es < eh || p + es > tend) break;
                            if (et == kTfhd) {
                                // tfhd: fullbox(4) + track_ID(4) + ...
                                if (p + eh + 8 <= tend) {
                                    trackId = Rd32(buf + p + eh + 4);
                                }
                                break;
                            }
                            p += (size_t)es;
                        }
                    }

                    // pass 2：找 tfdt 并改写
                    if (trackId != 0) {
                        size_t p = tpos;
                        while (p + 8 <= tend) {
                            uint64_t es = Rd32(buf + p);
                            uint32_t et = fourcc(buf + p + 4);
                            size_t   eh = 8;
                            if (es == 1) {
                                if (p + 16 > tend) break;
                                es = Rd64(buf + p + 8);
                                eh = 16;
                            }
                            if (es < eh || p + es > tend) break;
                            if (et == kTfdt) {
                                // tfdt: fullbox => version(1) + flags(3) + bmdt(4 or 8)
                                if (p + eh + 4 > tend) break;
                                uint8_t  ver  = buf[p + eh];
                                size_t   bmdtOff = p + eh + 4;
                                uint64_t bmdt = 0;
                                if (ver == 1) {
                                    if (bmdtOff + 8 > tend) break;
                                    bmdt = Rd64(buf + bmdtOff);
                                } else {
                                    if (bmdtOff + 4 > tend) break;
                                    bmdt = Rd32(buf + bmdtOff);
                                }

                                auto it = m_tfdtBase.find(trackId);
                                if (it == m_tfdtBase.end()) {
                                    m_tfdtBase.emplace(trackId, bmdt);
                                    it = m_tfdtBase.find(trackId);
                                    CLOG_INF("RollingRecorder tfdt anchor: track=%u base=%llu (file=%s)\n",
                                             trackId, (unsigned long long)bmdt, m_curFile.c_str());
                                }
                                uint64_t base = it->second;
                                uint64_t fixed = (bmdt >= base) ? (bmdt - base) : 0;
                                if (ver == 1) Wr64(buf + bmdtOff, fixed);
                                else          Wr32(buf + bmdtOff, (uint32_t)fixed);

                                // 抓取视频 track 的 tfdt 给 .idx 索引用
                                if (outFirstVideoTfdt && !firstVideoTfdtCaptured &&
                                    trackId == kVideoTrackId) {
                                    *outFirstVideoTfdt = fixed;
                                    firstVideoTfdtCaptured = true;
                                }
                                break;
                            }
                            p += (size_t)es;
                        }
                    }
                }
                mpos += (size_t)cs;
            }
        }

        pos += (size_t)bsize;
    }
}

// ---------------------------------------------------------------------------
// 伴生 .idx 索引文件
//
// 文件格式（极简 JSON，便于浏览器 fetch().json() 直接吃）：
//   {"v":1,"ts":90000,"init":<bytes>,"frags":[
//     [<tfdt90k>,<offset>,<size>],
//     ...
//   ]}
//
// 时基 90kHz 对应 fmp4Muxer 的 video time_base（kTbVideo90k），
// 视频 track 的 tfdt 单位即 1/90000 秒。
//
// 写入策略：增量 append，每个 fragment 写完原始 mp4 字节后追加一行索引。
// 中途读到的 .idx 可能是不完整的（最后没有 "]}"）—— 客户端解析失败时回退
// 到全量流式 fetch，用户体验上只是该片不能精准跳转。
// ---------------------------------------------------------------------------

void C_RollingRecorder::CloseIdxFile_locked()
{
    if (!m_idxOfs.is_open()) return;
    // 收尾："]\n}\n"，把不完整 JSON 补成完整对象
    m_idxOfs << "]}\n";
    m_idxOfs.flush();
    m_idxOfs.close();
    m_idxFirstFrag = true;
}

bool C_RollingRecorder::OpenIdxFile_locked(const std::string& idxFullPath)
{
    if (m_idxOfs.is_open()) {
        m_idxOfs.close();
    }
    m_idxOfs.open(idxFullPath, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!m_idxOfs.is_open()) return false;

    // 写头：包含版本、时基、init segment 字节数（客户端用此发 Range bytes=0-<init-1>）
    m_idxOfs << "{\"v\":1,\"ts\":90000,\"init\":"
             << m_initSeg.size()
             << ",\"frags\":[";
    m_idxFirstFrag = true;
    return m_idxOfs.good();
}

void C_RollingRecorder::AppendIdxEntry_locked(uint64_t tfdt90k,
                                              uint64_t byteOffset,
                                              uint32_t byteSize)
{
    if (!m_idxOfs.is_open()) return;
    if (!m_idxFirstFrag) m_idxOfs << ',';
    m_idxFirstFrag = false;
    m_idxOfs << '[' << tfdt90k << ',' << byteOffset << ',' << byteSize << ']';
    // 立即 flush 让正在录的最新片也能被回放页查询：
    //   .idx 内容此时不带尾部 "]}"，是个非法 JSON。但客户端会做容错：
    //   解析失败则尝试自己 append "]}" 再 parse，能拿到截止当前的所有 frag。
    m_idxOfs.flush();
}
