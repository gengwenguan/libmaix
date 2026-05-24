/*********************************************************************************
  *Copyright(C),Your Company
  *FileName:  snapshot.h
  *Author:    gengwenguan
  *Date:      2026-05-23
  *Description:  手动拍照模块（按需抓帧，零拷贝快路径）。
  *
  *  设计要点：
  *    - 独立目录（与录像分离）：snapshot/YYYYMMDD_HHMMSS_NNN.jpg（平铺，无子目录）
  *    - 触发方式：仅手动（HTTP API 或前端按钮）+ AI 自动拍照；不自动清理
  *    - 数据来源：Terminal 在 InputNv21 时调 OnNv21Frame()，但**只有在拍照请求挂起时**
  *      才真的 memcpy；其它时间一次原子 load 立即返回，对相机管线影响为零。
  *
  *  请求-应答模型（解决"每帧都拷贝"的浪费）：
  *      ┌────────────────┐                ┌────────────────────┐
  *      │ TakeOne()      │  set m_want=1  │ OnNv21Frame()      │
  *      │ (HTTP 线程)    │ ─────────────▶│ (相机回调线程)     │
  *      │  cv.wait()     │                │  m_want.load()? no │
  *      │                │  ◀─ notify ────│  yes → memcpy 写 m_pending
  *      │  swap m_pending│                │  m_want=0, notify  │
  *      │  encode + save │                │                    │
  *      └────────────────┘                └────────────────────┘
  *
  *  线程安全：
  *    - OnNv21Frame() 由相机回调线程（30Hz）调；TakeOne() 由 HTTP / PersonDetector
  *      线程低频调（< 1Hz）。
  *    - m_mu 仅保护 m_pending / m_want / m_haveFrame；编码和写盘都在锁外。
**********************************************************************************/
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class C_Snapshot
{
public:
    // width/height：相机原始 NV21 尺寸
    C_Snapshot(int width, int height);
    ~C_Snapshot();

    // 输出目录（拍照保存到这里）。会自动 mkdir -p。
    int Start(const std::string& outDir);

    // 由相机回调线程每帧调用。
    // 快路径：m_want=false 时只做一次 atomic load，立即返回（< 1ns）。
    // 慢路径：仅当有 TakeOne 在等帧时才进锁 memcpy 一次（460KB）并 notify。
    // 这样对 30Hz 主循环几乎零开销。
    void OnNv21Frame(const unsigned char* nv21);

    // 手动拍一张：编码 JPEG 写盘，返回保存的文件名（仅 basename）。
    // 内部会先 set m_want=true 等下一帧（最多 1s 超时）；超时返回空串。
    // 失败返回空串。outFullPath 可选，返回完整路径。
    std::string TakeOne(std::string* outFullPath = nullptr);

    // 自动淘汰：若目录内 .jpg 数量超过 maxCount，按 mtime 升序删除最旧的。
    int  PruneOldestIfOver(int maxCount);

    std::string OutDir() const { return m_outDir; }
    int         Width()  const { return m_width;  }
    int         Height() const { return m_height; }

private:
    // 把 NV21 编成 JPEG 字节流；成功返回 true。无锁版本，调用方保证 nv21 独占。
    bool EncodeNv21ToJpeg(const std::vector<unsigned char>& nv21,
                          std::vector<unsigned char>& jpeg);
    static bool MkdirP(const std::string& path);

private:
    int                       m_width;
    int                       m_height;
    std::string               m_outDir;

    // ---- 请求-应答 ----
    // 快路径用 atomic：相机线程每帧只读一次，relaxed 即可（不需要看到自己之前
    // 写的什么数据，只是判定"有没有挂起请求"，错过一帧就下一帧再来）。
    std::atomic<bool>         m_want{false};

    mutable std::mutex        m_mu;
    std::condition_variable   m_cv;
    std::vector<unsigned char> m_pending;     // 相机线程写 → TakeOne swap 走
    bool                       m_haveFrame = false; // m_pending 是否已被填充

    // 同一天内的递增序号（防止同一秒多张冲突）
    int  m_seqOfDay  = 0;
    int  m_lastDay   = 0;   // YYYYMMDD（int）
    std::mutex m_seqMu;     // 仅保护 m_seqOfDay/m_lastDay（与帧 buffer 分离）
};
