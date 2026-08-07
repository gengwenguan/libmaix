/*********************************************************************************
 *Copyright(C),Your Company
 *FileName:  appConfig.h
 *Author:    gengwenguan
 *Date:      2026-05-23
 *Description:  全局运行时配置中心。
 *
 *  设计要点：
 *    1. 单例（GetInst()），避免到处传参；只在主线程构造一次。
 *    2. JSON 文件持久化（<exeDir>/config.json）；启动时 Load，每次 Update 后 Save。
 *    3. POD 结构体快照：读取走 GetSnapshot()（mutex 内拷一份），写入走 ApplyPatch()。
 *
 *  统一约定：pull-on-demand
 *    各模块**不要持有 cfg 引用、不要在自己内部缓存配置副本**，
 *    每次决策点（每帧 / 每个 fragment / 每次 sweep / 每次 HTTP）调一次 GetSnapshot()
 *    即可。已验证开销 < 1us，30Hz 帧回调里也完全够用（参见 motionDetector.cpp）。
 *
 *    这样做的好处：
 *      - 单一真相源：配置只存在 m_snap 一处，不存在"模块副本与中心副本不同步"的可能；
 *      - 零同步成本：不需要 atomic、Subscribe、cv 唤醒等机制；
 *      - 改完即时生效：web 改完下一帧 / 下一片 / 下一次扫描就读到新值；
 *      - 看起来像"边沿事件"的场景（如 ai_enabled false→true 后要 reload model），
 *        用模块自身的状态机识别（personDetector 用 m_modelLoaded 标记），仍然是 pull。
 *
 *  为什么不引第三方 json 库：
 *    项目里 terminal.cpp 已用手写 JsonEscape + 字符串拼接，引入 nlohmann
 *    会加约 40KB 二进制，仅为几个标量字段不划算。这里 parser 也只支持
 *    "key": <number|true|false|"string"> 这种最朴素的形式，注释和嵌套对象都不支持。
**********************************************************************************/
#pragma once
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

class C_AppConfig
{
public:
    // 完整配置快照。所有字段都有合理默认值；进程从未配过文件也能跑。
    struct Snapshot {
        // ---- AI 检测（人形识别 → 自动拍照）----
        bool   ai_enabled        = false;   // 默认关闭，需用户在 web 上显式打开
        float  ai_threshold      = 0.6f;    // YOLO confidence 阈值（0.30~0.90），与 maixhub person_int8 推荐一致
        int    ai_min_interval_s = 5;       // 同一只人 N 秒内只触发一次
        int    ai_infer_fps      = 5;       // NPU 推理帧率（1~10）

        // ---- 录像 ----
        int      record_segment_s   = 600;                       // 单片时长（秒）
        int      record_retain_days = 7;                          // 保留天数
        uint64_t record_max_bytes   = 16ull * 1024 * 1024 * 1024; // 总容量上限

        // ---- 相册（手动 + 自动拍照共用）----
        int    album_max_photos = 1000;     // 超出按 mtime 删最老
        int    photo_jpeg_qual  = 88;       // JPEG 编码质量（50~95）

        // ---- 麦克风滤波 ----
        // 0=关闭；1=均衡推荐；2=激进；3=精细；4=极激进；5=均衡+噪声门
        int    mic_filter_mode = 0;

        // ---- 移动侦测（VMD：与 AI 检测平行的轻量级触发器）----
        // 原理：把 NV21 的 Y 平面下采样到 80×60，与上一参考帧做差，
        //       diff > pixel_thresh 的像素数占比超过 area_ratio 即判定"有移动"。
        // 触发：命中后调 Snapshot::TakeOne()；与 AI 检测互不干扰，可单独/同时启用。
        bool   vmd_enabled        = false;  // 默认关闭
        int    vmd_pixel_thresh   = 25;     // 单像素差阈值（0~255 luma）；越大越不敏感
        float  vmd_area_ratio     = 0.02f;  // 面积占比阈值（0~1）：0.02 = 2% 像素发生变化才认定为移动
        int    vmd_min_interval_s = 2;      // 同一时段最短重复拍照间隔（秒）；按用户要求默认 2s
        int    vmd_check_fps      = 5;      // 每秒做几次差分判定（1~30）；越高越费 CPU 但响应快

        // ---- OSD ----
        bool   osd_show_ip     = true;
        bool   osd_show_time   = true;
        bool   osd_show_ai_box = true;   // 是否在 cam0 画面上叠加 AI 检测框（仅 ai_enabled=true 时有意义）

        // ---- 外接补光灯（GPIO 开关，默认 PH13=237）----
        // 由 lightController 常驻线程按北京时间（UTC+8）+ 麦克风响度评估是否点亮。
        bool   light_enabled     = false;  // 总开关，默认关（关时确保灯灭并空转）
        int    light_mode        = 0;      // 0=时段内常亮；1=时段内声控触发
        int    light_start_hour  = 18;     // 点亮时段起始小时[0,23]；start==end 视为全天
        int    light_end_hour    = 6;      // 点亮时段结束小时[0,23]；支持跨零点（18->6=傍晚到清晨）
        int    light_sound_thresh= 35;     // 声控模式：麦克风响度阈值[0,100]，>=触发
        // 响度量程版本：1/缺失=原始 0~100；2=0~1000；3=低响度再放大 5 倍并封顶 100。
        int    light_sound_scale_version = 3;
        int    light_hold_s      = 30;     // 声控模式：触发后持续点亮秒数[1,3600]
        int    light_gpio        = 237;    // 控制脚 sysfs 编号（PH13=237；换脚免改代码）
        bool   light_active_low  = false;  // true=低电平点亮（兼容部分灯板）

        // ---- MQTT IPv6 地址上报 ----
        // 常驻检测本机某网卡的全局 IPv6 地址，变化时通过 MQTT 发布到指定 topic，
        // 供外部（手机 / 服务器）获取板子的公网 IPv6 直连地址。默认关闭，零开销。
        bool        mqtt_enabled     = false;             // 总开关，默认关
        std::string mqtt_broker_host = "broker.emqx.io";  // broker 地址（域名或 IP）
        int         mqtt_broker_port = 1883;              // 明文 MQTT 端口
        std::string mqtt_topic       = "cam/ipv6";        // 发布主题
        std::string mqtt_client_id   = "v831cam";         // 客户端 ID（多设备需区分时改）
        int         mqtt_poll_sec    = 10;                // IPv6 轮询周期（秒）
        std::string mqtt_iface       = "wlan0";           // 监测的网卡名
        // 保活重报周期（秒）：即使地址没变，超过此间隔也强制重报一次（心跳，
        // 让订阅端感知设备存活、新订阅者能拿到当前地址）。0 = 关闭保活，仅变化时报。
        int         mqtt_report_interval_s = 3600;        // 默认 1 小时
        // 是否让 broker 保留消息（retain）：新订阅者一连上即收到最后一次上报的地址。
        // 对"当前 IPv6"这类状态语义很合适，默认开启。
        bool        mqtt_retain      = true;
    };

public:
    static C_AppConfig& GetInst();

    // 初始化：传入 config.json 完整路径。会尝试读盘；不存在则使用默认值并立即写出一份。
    // 多次调用只第一次生效。
    void Init(const std::string& path);

    // 取一份 snapshot（线程安全）。各模块每个决策点调一次即可，开销 < 1us。
    Snapshot GetSnapshot() const;

    // 局部更新：传入若干 (key, value) 字符串对，**仅更新存在的键**。
    // 非法 key 直接忽略并记 warning；非法 value（如 ai_threshold=2.0）会被 clamp 到合法区间。
    // 返回更新后的完整 snapshot；同时写盘。
    Snapshot ApplyPatch(const std::vector<std::pair<std::string, std::string>>& kv);

    // 把 snapshot 序列化成 JSON 字符串（紧凑格式，给 /api/config 用）
    static std::string ToJson(const Snapshot& s);

    // 配置文件路径（已 Init 后才有效）
    std::string Path() const { std::lock_guard<std::mutex> lk(m_mu); return m_path; }

private:
    C_AppConfig() = default;
    C_AppConfig(const C_AppConfig&) = delete;
    C_AppConfig& operator=(const C_AppConfig&) = delete;

    bool LoadFromFile_locked();
    bool SaveToFile_locked() const;
    static bool ParseJsonObject(const std::string& text,
                                std::vector<std::pair<std::string,std::string>>& kv);
    static void ClampSnapshot(Snapshot& s);

    // 把单个 (key, value) 字符串赋到 snapshot 的对应字段；未识别 key 返回 false。
    static bool AssignKv(Snapshot& s, const std::string& k, const std::string& v);

private:
    mutable std::mutex  m_mu;
    bool                m_inited = false;
    bool                m_needsSave = false;  // Load 时完成旧配置迁移后，Init 负责一次性回写
    std::string         m_path;
    Snapshot            m_snap;     // 当前生效的配置
};
