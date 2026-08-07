/*********************************************************************************
  *Copyright(C),Your Company
  *FileName:  actionStore.cpp
  *Author:    gengwenguan
  *Date:      2026-07-18
  *Description:  "设备动作"配置存储实现。详见 actionStore.h。
**********************************************************************************/
#include "actionStore.h"
#include "logAdapt.h"

#include <cctype>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>

namespace {

inline std::string Trim(const std::string& s)
{
    size_t b = 0, e = s.size();
    while (b < e && std::isspace((unsigned char)s[b])) ++b;
    while (e > b && std::isspace((unsigned char)s[e - 1])) --e;
    return s.substr(b, e - b);
}

// JSON 转义（与 terminal.cpp::JsonEscape 同规则，独立一份避免跨模块耦合）
std::string JsonEscape(const std::string& s)
{
    std::string r;
    r.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '\\': r += "\\\\"; break;
            case '"':  r += "\\\""; break;
            case '\n': r += "\\n";  break;
            case '\r': r += "\\r";  break;
            case '\t': r += "\\t";  break;
            default:   r += c;
        }
    }
    return r;
}

// 从 [pos, end) 里读下一个带引号的 JSON 字符串（支持 \" \\ 转义）。
// 成功返回 true：out 填内容，pos 移到收尾引号之后。
bool ReadJsonString(const std::string& t, size_t& pos, std::string& out)
{
    size_t n = t.size();
    while (pos < n && std::isspace((unsigned char)t[pos])) ++pos;
    if (pos >= n || t[pos] != '"') return false;
    ++pos;
    out.clear();
    while (pos < n) {
        char c = t[pos++];
        if (c == '"') return true;
        if (c == '\\') {
            if (pos >= n) return false;
            char e = t[pos++];
            switch (e) {
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case '"': out += '"';  break;
                case '\\':out += '\\'; break;
                case '/': out += '/';  break;
                default:  out += e;    break;
            }
        } else {
            out += c;
        }
    }
    return false;  // 未闭合
}

// 仅保留可安全展示的名称：去掉控制字符，长度截断由上层负责。
bool HasControlChar(const std::string& s)
{
    for (unsigned char c : s) if (c < 0x20) return true;
    return false;
}

} // namespace

void C_ActionStore::Init(const std::string& path)
{
    std::lock_guard<std::mutex> lk(m_mu);
    if (m_inited) return;
    m_inited = true;
    m_path   = path;
    if (LoadFromFile_locked()) {
        CLOG_INF("ActionStore: loaded %zu actions from %s\n",
                 m_actions.size(), m_path.c_str());
    } else {
        CLOG_INF("ActionStore: %s not found/empty, start empty\n", m_path.c_str());
    }
}

std::vector<C_ActionStore::Action> C_ActionStore::List() const
{
    std::lock_guard<std::mutex> lk(m_mu);
    return m_actions;
}

bool C_ActionStore::Get(const std::string& id, Action& out) const
{
    std::lock_guard<std::mutex> lk(m_mu);
    for (const auto& a : m_actions) {
        if (a.id == id) { out = a; return true; }
    }
    return false;
}

bool C_ActionStore::IsValidUrl(const std::string& url)
{
    if (url.size() < 8 || url.size() > kMaxUrlLen) return false;   // "http://x"
    // 仅支持明文 http://（大小写不敏感）
    const char* p = "http://";
    for (int i = 0; i < 7; ++i) {
        if (std::tolower((unsigned char)url[i]) != p[i]) return false;
    }
    if (url.size() <= 7) return false;  // http:// 后必须有 host
    if (HasControlChar(url)) return false;
    return true;
}

std::string C_ActionStore::GenIdLocked()
{
    // 时间戳 + 递增序号，够稳定且人可读；不追求全局唯一，只要本文件内不撞。
    ++m_seq;
    char buf[48];
    std::snprintf(buf, sizeof(buf), "a%ld_%llu",
                  (long)time(nullptr), (unsigned long long)m_seq);
    return std::string(buf);
}

bool C_ActionStore::Add(const std::string& nameIn, const std::string& urlIn,
                        std::string& outId, std::string& errOut)
{
    std::string name = Trim(nameIn);
    std::string url  = Trim(urlIn);

    if (name.empty() || name.size() > kMaxNameLen || HasControlChar(name)) {
        errOut = "bad name";
        return false;
    }
    if (!IsValidUrl(url)) {
        errOut = "bad url";
        return false;
    }

    std::lock_guard<std::mutex> lk(m_mu);
    if (m_actions.size() >= kMaxActions) {
        errOut = "too many actions";
        return false;
    }
    Action a;
    a.id   = GenIdLocked();
    a.name = name;
    a.url  = url;
    m_actions.push_back(a);
    SaveToFile_locked();
    outId = a.id;
    return true;
}

bool C_ActionStore::Remove(const std::string& id)
{
    std::lock_guard<std::mutex> lk(m_mu);
    for (size_t i = 0; i < m_actions.size(); ++i) {
        if (m_actions[i].id == id) {
            m_actions.erase(m_actions.begin() + i);
            SaveToFile_locked();
            return true;
        }
    }
    return false;
}

bool C_ActionStore::Update(const std::string& id, const std::string& nameIn,
                           const std::string& urlIn, std::string& errOut)
{
    std::string name = Trim(nameIn);
    std::string url  = Trim(urlIn);

    if (name.empty() || name.size() > kMaxNameLen || HasControlChar(name)) {
        errOut = "bad name";
        return false;
    }
    if (!IsValidUrl(url)) {
        errOut = "bad url";
        return false;
    }

    std::lock_guard<std::mutex> lk(m_mu);
    for (auto& a : m_actions) {
        if (a.id == id) {
            a.name = name;
            a.url  = url;
            SaveToFile_locked();
            return true;
        }
    }
    errOut = "not found";
    return false;
}

std::string C_ActionStore::ToJson() const
{
    std::lock_guard<std::mutex> lk(m_mu);
    std::ostringstream js;
    js << "[";
    for (size_t i = 0; i < m_actions.size(); ++i) {
        if (i) js << ",";
        js << "{\"id\":\""    << JsonEscape(m_actions[i].id)
           << "\",\"name\":\""<< JsonEscape(m_actions[i].name)
           << "\",\"url\":\"" << JsonEscape(m_actions[i].url)
           << "\"}";
    }
    js << "]";
    return js.str();
}

// 极简数组解析：只认 [{"id":"..","name":"..","url":".."}, ...] 这种由本模块
// 自己写出的形态。字段顺序不敏感；多余字段跳过；解析失败视为空列表（不致命）。
bool C_ActionStore::LoadFromFile_locked()
{
    std::ifstream ifs(m_path);
    if (!ifs.is_open()) return false;
    std::ostringstream ss; ss << ifs.rdbuf();
    const std::string t = ss.str();

    size_t i = 0, n = t.size();
    auto skipWs = [&]() { while (i < n && std::isspace((unsigned char)t[i])) ++i; };

    skipWs();
    if (i >= n || t[i] != '[') return false;
    ++i;

    m_actions.clear();
    while (true) {
        skipWs();
        if (i >= n) break;
        if (t[i] == ']') { ++i; break; }
        if (t[i] == ',') { ++i; continue; }
        if (t[i] != '{') break;  // 结构不符，停止解析（保留已读到的）
        ++i;

        Action a;
        // 逐个 "key": "value" 读到本对象的 '}'
        while (true) {
            skipWs();
            if (i >= n) break;
            if (t[i] == '}') { ++i; break; }
            if (t[i] == ',') { ++i; continue; }

            std::string key;
            if (!ReadJsonString(t, i, key)) { i = n; break; }
            skipWs();
            if (i >= n || t[i] != ':') { i = n; break; }
            ++i;
            std::string val;
            if (!ReadJsonString(t, i, val)) { i = n; break; }

            if      (key == "id")   a.id   = val;
            else if (key == "name") a.name = val;
            else if (key == "url")  a.url  = val;
            // 其它 key 忽略
        }

        // 只收合法项；顺带修正历史脏数据
        if (!a.name.empty() && a.name.size() <= kMaxNameLen &&
            !HasControlChar(a.name) && IsValidUrl(a.url)) {
            if (a.id.empty()) a.id = GenIdLocked();
            if (m_actions.size() < kMaxActions) m_actions.push_back(a);
        }
    }
    return true;
}

bool C_ActionStore::SaveToFile_locked() const
{
    std::ofstream ofs(m_path, std::ios::trunc);
    if (!ofs.is_open()) {
        CLOG_ERR("ActionStore: open %s for write failed\n", m_path.c_str());
        return false;
    }
    ofs << "[\n";
    for (size_t i = 0; i < m_actions.size(); ++i) {
        ofs << "  {\"id\":\""    << JsonEscape(m_actions[i].id)
            << "\",\"name\":\""  << JsonEscape(m_actions[i].name)
            << "\",\"url\":\""   << JsonEscape(m_actions[i].url)
            << "\"}";
        ofs << (i + 1 < m_actions.size() ? ",\n" : "\n");
    }
    ofs << "]\n";
    ofs.close();
    return true;
}
