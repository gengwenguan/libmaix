/*********************************************************************************
  *Copyright(C),Your Company
  *FileName:  actionStore.h
  *Author:    gengwenguan
  *Date:      2026-07-18
  *Description:  "设备动作"配置存储（<exeDir>/actions.json）。
  *
  *  用途：
  *    web 上可动态增删的一组"按钮 → URL"映射（开门 / 开灯 / 关灯 …）。
  *    用户点某个按钮时，前端调 /api/actions/invoke，camera 后端就到对应 URL
  *    发一次出站 POST（见 httpClient）。
  *
  *  为什么不塞进 AppConfig：
  *    AppConfig 的极简 parser 只支持顶层扁平标量（"key": <num|bool|str>），
  *    不支持数组 / 对象。动作是一个 [{id,name,url}] 列表，需要独立的存储与
  *    数组解析，因此单开一个模块，持久化到 <exeDir>/actions.json。
  *
  *  线程安全：
  *    HTTP 客户端线程并发调用（增删查），内部用 m_mu 全程串行保护 + 每次写盘。
  *    与项目其它模块一致：文件小、频率低（人手点击级），直接锁内落盘即可。
**********************************************************************************/
#pragma once
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

class C_ActionStore
{
public:
    struct Action {
        std::string id;    // 稳定标识（后端生成，前端删除/触发时回传）
        std::string name;  // 按钮显示名（用户填）
        std::string url;   // 目标 http:// URL（用户填）
    };

    // 数量与长度上限：防止 actions.json 被写爆 / 前端渲染过多按钮。
    static const size_t kMaxActions  = 32;
    static const size_t kMaxNameLen  = 32;
    static const size_t kMaxUrlLen   = 512;

    C_ActionStore() = default;

    // 传入 actions.json 完整路径。会尝试读盘；文件不存在视为空列表（不报错）。
    // 多次调用只第一次生效。
    void Init(const std::string& path);

    // 取全部动作快照（线程安全）。
    std::vector<Action> List() const;

    // 按 id 查一个；找到返回 true 并填 out。
    bool Get(const std::string& id, Action& out) const;

    // 新增一个动作。name/url 会被 trim + 校验（url 必须 http:// 开头）。
    //   成功：返回 true 并把新 id 填入 outId；写盘。
    //   失败：返回 false 并把英文原因填入 errOut（数量超限 / 字段非法）。
    bool Add(const std::string& name, const std::string& url,
             std::string& outId, std::string& errOut);

    // 按 id 删除；删除了返回 true（并写盘），不存在返回 false。
    bool Remove(const std::string& id);

    // 按 id 更新 name/url（校验规则同 Add）。
    //   成功：返回 true 并写盘。
    //   失败：返回 false 并把英文原因填入 errOut（id 不存在 / 字段非法）。
    bool Update(const std::string& id, const std::string& name,
                const std::string& url, std::string& errOut);

    // 序列化成 JSON 数组字符串：[{"id":"..","name":"..","url":".."}, ...]
    std::string ToJson() const;

    // URL 合法性校验（静态，供上层复用）：非空、http:// 开头、长度受限、无控制字符。
    static bool IsValidUrl(const std::string& url);

private:
    bool LoadFromFile_locked();
    bool SaveToFile_locked() const;
    std::string GenIdLocked();

private:
    mutable std::mutex  m_mu;
    bool                m_inited = false;
    std::string         m_path;
    std::vector<Action> m_actions;
    uint64_t            m_seq = 0;   // 递增序号，参与 id 生成
};
