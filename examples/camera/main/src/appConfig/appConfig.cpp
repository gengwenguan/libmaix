/*********************************************************************************
 *Copyright(C),Your Company
 *FileName:  appConfig.cpp
 *Author:    gengwenguan
 *Date:      2026-05-23
 *Description:  全局运行时配置中心实现。详见 appConfig.h 顶部注释。
**********************************************************************************/
#include "appConfig.h"
#include "logAdapt.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace {

// 把 [v_min, v_max] 之外的 v 拉回区间内
template <typename T>
inline T Clamp(T v, T v_min, T v_max) {
    if (v < v_min) return v_min;
    if (v > v_max) return v_max;
    return v;
}

inline std::string Trim(const std::string& s)
{
    size_t b = 0, e = s.size();
    while (b < e && std::isspace((unsigned char)s[b])) ++b;
    while (e > b && std::isspace((unsigned char)s[e - 1])) --e;
    return s.substr(b, e - b);
}

inline bool StrToBool(const std::string& v, bool def)
{
    std::string t = Trim(v);
    if (t == "true" || t == "1" || t == "yes" || t == "on")  return true;
    if (t == "false"|| t == "0" || t == "no"  || t == "off") return false;
    return def;
}

inline int StrToInt(const std::string& v, int def)
{
    try { return std::stoi(Trim(v)); } catch (...) { return def; }
}

inline uint64_t StrToU64(const std::string& v, uint64_t def)
{
    try { return (uint64_t)std::stoull(Trim(v)); } catch (...) { return def; }
}

inline float StrToFloat(const std::string& v, float def)
{
    try { return std::stof(Trim(v)); } catch (...) { return def; }
}

} // namespace

// =====================================================================
C_AppConfig& C_AppConfig::GetInst()
{
    static C_AppConfig inst;
    return inst;
}

void C_AppConfig::Init(const std::string& path)
{
    std::lock_guard<std::mutex> lk(m_mu);
    if (m_inited) return;
    m_inited = true;
    m_path   = path;

    if (LoadFromFile_locked()) {
        CLOG_INF("AppConfig: loaded from %s\n", m_path.c_str());
    } else {
        CLOG_INF("AppConfig: %s not found, using defaults\n", m_path.c_str());
        // 第一次启动写一份默认配置出来，便于用户离线编辑
        SaveToFile_locked();
    }
    ClampSnapshot(m_snap);
    // 旧配置迁移完成后立刻回写版本标记，保证只放大一次；写失败不影响本次运行。
    if (m_needsSave) {
        SaveToFile_locked();
        m_needsSave = false;
    }
}

C_AppConfig::Snapshot C_AppConfig::GetSnapshot() const
{
    std::lock_guard<std::mutex> lk(m_mu);
    return m_snap;
}

C_AppConfig::Snapshot C_AppConfig::ApplyPatch(
    const std::vector<std::pair<std::string,std::string>>& kv)
{
    Snapshot newSnap;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        newSnap = m_snap;
        for (const auto& p : kv) {
            if (!AssignKv(newSnap, p.first, p.second)) {
                CLOG_INF("AppConfig: ignore unknown key=%s val=%s\n",
                         p.first.c_str(), p.second.c_str());
            }
        }
        ClampSnapshot(newSnap);
        m_snap = newSnap;
        SaveToFile_locked();
    }
    // 注意：本项目统一约定 pull-on-demand，模块自己每次决策时调 GetSnapshot()
    // 即可读到新值，不需要在这里向订阅者推送。
    return newSnap;
}

std::string C_AppConfig::ToJson(const Snapshot& s)
{
    std::ostringstream js;
    js << "{"
       << "\"ai_enabled\":"        << (s.ai_enabled ? "true" : "false")
       << ",\"ai_threshold\":"     << s.ai_threshold
       << ",\"ai_min_interval_s\":"<< s.ai_min_interval_s
       << ",\"ai_infer_fps\":"     << s.ai_infer_fps
       << ",\"record_segment_s\":" << s.record_segment_s
       << ",\"record_retain_days\":" << s.record_retain_days
       << ",\"record_max_bytes\":" << s.record_max_bytes
       << ",\"album_max_photos\":" << s.album_max_photos
       << ",\"photo_jpeg_qual\":"  << s.photo_jpeg_qual
       << ",\"mic_filter_mode\":"  << s.mic_filter_mode
       << ",\"vmd_enabled\":"        << (s.vmd_enabled ? "true" : "false")
       << ",\"vmd_pixel_thresh\":"   << s.vmd_pixel_thresh
       << ",\"vmd_area_ratio\":"     << s.vmd_area_ratio
       << ",\"vmd_min_interval_s\":" << s.vmd_min_interval_s
       << ",\"vmd_check_fps\":"      << s.vmd_check_fps
       << ",\"osd_show_ip\":"      << (s.osd_show_ip   ? "true" : "false")
       << ",\"osd_show_time\":"    << (s.osd_show_time ? "true" : "false")
       << ",\"osd_show_ai_box\":"  << (s.osd_show_ai_box ? "true" : "false")
       << ",\"light_enabled\":"     << (s.light_enabled ? "true" : "false")
       << ",\"light_mode\":"        << s.light_mode
       << ",\"light_start_hour\":"  << s.light_start_hour
       << ",\"light_end_hour\":"    << s.light_end_hour
       << ",\"light_sound_thresh\":"<< s.light_sound_thresh
       << ",\"light_sound_scale_version\":" << s.light_sound_scale_version
       << ",\"light_hold_s\":"      << s.light_hold_s
       << ",\"light_gpio\":"        << s.light_gpio
       << ",\"light_active_low\":"  << (s.light_active_low ? "true" : "false")
       << ",\"mqtt_enabled\":"     << (s.mqtt_enabled ? "true" : "false")
       << ",\"mqtt_broker_host\":\"" << s.mqtt_broker_host << "\""
       << ",\"mqtt_broker_port\":" << s.mqtt_broker_port
       << ",\"mqtt_topic\":\""     << s.mqtt_topic << "\""
       << ",\"mqtt_client_id\":\"" << s.mqtt_client_id << "\""
       << ",\"mqtt_poll_sec\":"    << s.mqtt_poll_sec
       << ",\"mqtt_iface\":\""     << s.mqtt_iface << "\""
       << ",\"mqtt_report_interval_s\":" << s.mqtt_report_interval_s
       << ",\"mqtt_retain\":"      << (s.mqtt_retain ? "true" : "false")
       << "}";
    return js.str();
}

bool C_AppConfig::AssignKv(Snapshot& s, const std::string& k, const std::string& v)
{
    if      (k == "ai_enabled")         s.ai_enabled        = StrToBool(v, s.ai_enabled);
    else if (k == "ai_threshold")       s.ai_threshold      = StrToFloat(v, s.ai_threshold);
    else if (k == "ai_min_interval_s")  s.ai_min_interval_s = StrToInt(v,  s.ai_min_interval_s);
    else if (k == "ai_infer_fps")       s.ai_infer_fps      = StrToInt(v,  s.ai_infer_fps);
    else if (k == "record_segment_s")   s.record_segment_s  = StrToInt(v,  s.record_segment_s);
    else if (k == "record_retain_days") s.record_retain_days= StrToInt(v,  s.record_retain_days);
    else if (k == "record_max_bytes")   s.record_max_bytes  = StrToU64(v,  s.record_max_bytes);
    else if (k == "album_max_photos")   s.album_max_photos  = StrToInt(v,  s.album_max_photos);
    else if (k == "photo_jpeg_qual")    s.photo_jpeg_qual   = StrToInt(v,  s.photo_jpeg_qual);
    else if (k == "mic_filter_mode")    s.mic_filter_mode   = StrToInt(v,  s.mic_filter_mode);
    else if (k == "vmd_enabled")        s.vmd_enabled       = StrToBool(v, s.vmd_enabled);
    else if (k == "vmd_pixel_thresh")   s.vmd_pixel_thresh  = StrToInt(v,  s.vmd_pixel_thresh);
    else if (k == "vmd_area_ratio")     s.vmd_area_ratio    = StrToFloat(v,s.vmd_area_ratio);
    else if (k == "vmd_min_interval_s") s.vmd_min_interval_s= StrToInt(v,  s.vmd_min_interval_s);
    else if (k == "vmd_check_fps")      s.vmd_check_fps     = StrToInt(v,  s.vmd_check_fps);
    else if (k == "osd_show_ip")        s.osd_show_ip       = StrToBool(v, s.osd_show_ip);
    else if (k == "osd_show_time")      s.osd_show_time     = StrToBool(v, s.osd_show_time);
    else if (k == "osd_show_ai_box")    s.osd_show_ai_box   = StrToBool(v, s.osd_show_ai_box);
    else if (k == "light_enabled")      s.light_enabled     = StrToBool(v, s.light_enabled);
    else if (k == "light_mode")         s.light_mode        = StrToInt(v,  s.light_mode);
    else if (k == "light_start_hour")   s.light_start_hour  = StrToInt(v,  s.light_start_hour);
    else if (k == "light_end_hour")     s.light_end_hour    = StrToInt(v,  s.light_end_hour);
    else if (k == "light_sound_thresh") s.light_sound_thresh= StrToInt(v,  s.light_sound_thresh);
    else if (k == "light_sound_scale_version") s.light_sound_scale_version = StrToInt(v, s.light_sound_scale_version);
    else if (k == "light_hold_s")       s.light_hold_s      = StrToInt(v,  s.light_hold_s);
    else if (k == "light_gpio")         s.light_gpio        = StrToInt(v,  s.light_gpio);
    else if (k == "light_active_low")   s.light_active_low  = StrToBool(v, s.light_active_low);
    else if (k == "mqtt_enabled")       s.mqtt_enabled      = StrToBool(v, s.mqtt_enabled);
    else if (k == "mqtt_broker_host")   s.mqtt_broker_host  = Trim(v);
    else if (k == "mqtt_broker_port")   s.mqtt_broker_port  = StrToInt(v,  s.mqtt_broker_port);
    else if (k == "mqtt_topic")         s.mqtt_topic        = Trim(v);
    else if (k == "mqtt_client_id")     s.mqtt_client_id    = Trim(v);
    else if (k == "mqtt_poll_sec")      s.mqtt_poll_sec     = StrToInt(v,  s.mqtt_poll_sec);
    else if (k == "mqtt_iface")         s.mqtt_iface        = Trim(v);
    else if (k == "mqtt_report_interval_s") s.mqtt_report_interval_s = StrToInt(v, s.mqtt_report_interval_s);
    else if (k == "mqtt_retain")        s.mqtt_retain       = StrToBool(v, s.mqtt_retain);
    else return false;
    return true;
}

void C_AppConfig::ClampSnapshot(Snapshot& s)
{
    s.ai_threshold       = Clamp(s.ai_threshold,      0.10f,  0.95f);
    s.ai_min_interval_s  = Clamp(s.ai_min_interval_s, 1,      3600);
    s.ai_infer_fps       = Clamp(s.ai_infer_fps,      1,      30);
    s.record_segment_s   = Clamp(s.record_segment_s,  10,     3600);
    s.record_retain_days = Clamp(s.record_retain_days,1,      365);
    s.record_max_bytes   = Clamp<uint64_t>(s.record_max_bytes,
                                           64ull * 1024 * 1024,         // 下限 64MB
                                           512ull * 1024 * 1024 * 1024); // 上限 512GB
    s.album_max_photos   = Clamp(s.album_max_photos,  10,     100000);
    s.photo_jpeg_qual    = Clamp(s.photo_jpeg_qual,   30,     100);
    s.mic_filter_mode    = Clamp(s.mic_filter_mode,   0,      5);
    s.vmd_pixel_thresh   = Clamp(s.vmd_pixel_thresh,  1,      255);
    s.vmd_area_ratio     = Clamp(s.vmd_area_ratio,    0.001f, 0.5f);
    s.vmd_min_interval_s = Clamp(s.vmd_min_interval_s,1,      3600);
    s.vmd_check_fps      = Clamp(s.vmd_check_fps,     1,      30);
    s.light_mode         = Clamp(s.light_mode,        0,      1);
    s.light_start_hour   = Clamp(s.light_start_hour,  0,      23);
    s.light_end_hour     = Clamp(s.light_end_hour,    0,      23);
    s.light_sound_thresh = Clamp(s.light_sound_thresh,0,      100);
    s.light_sound_scale_version = 3;  // 当前唯一合法量程版本
    s.light_hold_s       = Clamp(s.light_hold_s,      1,      3600);
    // GPIO 编号：V831 主 PIO(pio) base=0 ngpio=288，合法范围 [0,287]；越界回退默认 237(PH13)
    if (s.light_gpio < 0 || s.light_gpio > 287) s.light_gpio = 237;
    s.mqtt_broker_port   = Clamp(s.mqtt_broker_port,  1,      65535);
    s.mqtt_poll_sec      = Clamp(s.mqtt_poll_sec,     2,      3600);
    // 0 = 关闭保活；非 0 时下限 60s（避免误配成几秒频繁重报），上限 24h
    if (s.mqtt_report_interval_s != 0)
        s.mqtt_report_interval_s = Clamp(s.mqtt_report_interval_s, 60, 86400);
}

bool C_AppConfig::LoadFromFile_locked()
{
    std::ifstream ifs(m_path);
    if (!ifs.is_open()) return false;
    std::ostringstream ss; ss << ifs.rdbuf();
    std::string text = ss.str();

    std::vector<std::pair<std::string,std::string>> kv;
    if (!ParseJsonObject(text, kv)) {
        CLOG_ERR("AppConfig: parse %s failed; reverting to defaults\n", m_path.c_str());
        return false;
    }
    bool hasSoundThreshold = false;
    bool hasSoundScaleVersion = false;
    for (const auto& p : kv) {
        if (p.first == "light_sound_thresh") hasSoundThreshold = true;
        if (p.first == "light_sound_scale_version") hasSoundScaleVersion = true;
        if (!AssignKv(m_snap, p.first, p.second)) {
            CLOG_INF("AppConfig: skip unknown key in %s: %s\n",
                     m_path.c_str(), p.first.c_str());
        }
    }

    // v3 在 v2 的低响度结果上再放大 5 倍，并把公开范围封顶为 100。
    // v2 阈值乘 5；v1/无版本相对 v3 的总倍率是 50。只有配置文件确实
    // 保存过旧阈值时才换算；没有该字段则保留当前默认值 35。
    const int loadedScaleVersion = hasSoundScaleVersion
        ? m_snap.light_sound_scale_version : 1;
    if (loadedScaleVersion < 3) {
        if (hasSoundThreshold) {
            const int oldThresh = m_snap.light_sound_thresh;
            const int multiplier = loadedScaleVersion < 2 ? 50 : 5;
            const long long scaled = (long long)oldThresh * multiplier;
            m_snap.light_sound_thresh = (int)Clamp<long long>(scaled, 0, 100);
            CLOG_INF("AppConfig: migrate light sound threshold %d -> %d "
                     "(scale v%d -> v3)\n",
                     oldThresh, m_snap.light_sound_thresh, loadedScaleVersion);
        }
        m_snap.light_sound_scale_version = 3;
        m_needsSave = true;
    }
    return true;
}

bool C_AppConfig::SaveToFile_locked() const
{
    std::ofstream ofs(m_path, std::ios::trunc);
    if (!ofs.is_open()) {
        CLOG_ERR("AppConfig: open %s for write failed\n", m_path.c_str());
        return false;
    }
    // 美化格式（一行一项），便于人读 / vi 修改
    ofs << "{\n"
        << "  \"ai_enabled\":         " << (m_snap.ai_enabled ? "true" : "false") << ",\n"
        << "  \"ai_threshold\":       " << m_snap.ai_threshold << ",\n"
        << "  \"ai_min_interval_s\":  " << m_snap.ai_min_interval_s << ",\n"
        << "  \"ai_infer_fps\":       " << m_snap.ai_infer_fps << ",\n"
        << "  \"record_segment_s\":   " << m_snap.record_segment_s << ",\n"
        << "  \"record_retain_days\": " << m_snap.record_retain_days << ",\n"
        << "  \"record_max_bytes\":   " << m_snap.record_max_bytes << ",\n"
        << "  \"album_max_photos\":   " << m_snap.album_max_photos << ",\n"
        << "  \"photo_jpeg_qual\":    " << m_snap.photo_jpeg_qual << ",\n"
        << "  \"mic_filter_mode\":    " << m_snap.mic_filter_mode << ",\n"
        << "  \"vmd_enabled\":        " << (m_snap.vmd_enabled ? "true" : "false") << ",\n"
        << "  \"vmd_pixel_thresh\":   " << m_snap.vmd_pixel_thresh << ",\n"
        << "  \"vmd_area_ratio\":     " << m_snap.vmd_area_ratio << ",\n"
        << "  \"vmd_min_interval_s\": " << m_snap.vmd_min_interval_s << ",\n"
        << "  \"vmd_check_fps\":      " << m_snap.vmd_check_fps << ",\n"
        << "  \"osd_show_ip\":        " << (m_snap.osd_show_ip   ? "true" : "false") << ",\n"
        << "  \"osd_show_time\":      " << (m_snap.osd_show_time ? "true" : "false") << ",\n"
        << "  \"osd_show_ai_box\":    " << (m_snap.osd_show_ai_box ? "true" : "false") << ",\n"
        << "  \"light_enabled\":      " << (m_snap.light_enabled ? "true" : "false") << ",\n"
        << "  \"light_mode\":         " << m_snap.light_mode << ",\n"
        << "  \"light_start_hour\":   " << m_snap.light_start_hour << ",\n"
        << "  \"light_end_hour\":     " << m_snap.light_end_hour << ",\n"
        << "  \"light_sound_thresh\": " << m_snap.light_sound_thresh << ",\n"
        << "  \"light_sound_scale_version\": " << m_snap.light_sound_scale_version << ",\n"
        << "  \"light_hold_s\":       " << m_snap.light_hold_s << ",\n"
        << "  \"light_gpio\":         " << m_snap.light_gpio << ",\n"
        << "  \"light_active_low\":   " << (m_snap.light_active_low ? "true" : "false") << ",\n"
        << "  \"mqtt_enabled\":       " << (m_snap.mqtt_enabled ? "true" : "false") << ",\n"
        << "  \"mqtt_broker_host\":   \"" << m_snap.mqtt_broker_host << "\",\n"
        << "  \"mqtt_broker_port\":   " << m_snap.mqtt_broker_port << ",\n"
        << "  \"mqtt_topic\":         \"" << m_snap.mqtt_topic << "\",\n"
        << "  \"mqtt_client_id\":     \"" << m_snap.mqtt_client_id << "\",\n"
        << "  \"mqtt_poll_sec\":      " << m_snap.mqtt_poll_sec << ",\n"
        << "  \"mqtt_iface\":         \"" << m_snap.mqtt_iface << "\",\n"
        << "  \"mqtt_report_interval_s\": " << m_snap.mqtt_report_interval_s << ",\n"
        << "  \"mqtt_retain\":        " << (m_snap.mqtt_retain ? "true" : "false") << "\n"
        << "}\n";
    ofs.close();
    return true;
}

// 极简 JSON 平坦对象解析：只支持顶层 { "key": <bool|number|"str">, ... }，
// 不支持嵌套对象/数组、不支持注释、不支持转义（值里如果带引号会失败）。
// 对我们 11 个标量字段足够。
bool C_AppConfig::ParseJsonObject(const std::string& text,
                                  std::vector<std::pair<std::string,std::string>>& kv)
{
    kv.clear();
    size_t i = 0, n = text.size();
    auto skipWs = [&](){
        while (i < n && std::isspace((unsigned char)text[i])) ++i;
    };
    skipWs();
    if (i >= n || text[i] != '{') return false;
    ++i;

    while (true) {
        skipWs();
        if (i >= n) return false;
        if (text[i] == '}') { ++i; break; }
        if (text[i] == ',') { ++i; continue; }

        // 解析 key（必须是带引号的字符串）
        if (text[i] != '"') return false;
        ++i;
        size_t kBeg = i;
        while (i < n && text[i] != '"') ++i;
        if (i >= n) return false;
        std::string key = text.substr(kBeg, i - kBeg);
        ++i; // skip closing "

        skipWs();
        if (i >= n || text[i] != ':') return false;
        ++i;
        skipWs();

        std::string val;
        if (i < n && text[i] == '"') {
            // 字符串值
            ++i;
            size_t vBeg = i;
            while (i < n && text[i] != '"') ++i;
            if (i >= n) return false;
            val = text.substr(vBeg, i - vBeg);
            ++i;
        } else {
            // 数字 / true / false / null —— 一直读到下一个 ',' '}' 或空白
            size_t vBeg = i;
            while (i < n && text[i] != ',' && text[i] != '}'
                   && !std::isspace((unsigned char)text[i])) ++i;
            val = text.substr(vBeg, i - vBeg);
        }
        kv.emplace_back(std::move(key), std::move(val));
    }
    return true;
}
