# Camera —— V831 / M2dock 全栈监控例程

基于 libmaix 框架，运行在 **M2dock（V831）** 开发板上的一体化网络监控应用。
覆盖：双路摄像头采集 → 硬件 H264 + AAC 编码 → fMP4 封装 → WebSocket 直播 →
HTTP/HTTPS 录像回放 → AI 人形识别自动抓拍 → 浏览器对讲（OPUS 下行）→
Web 配置中心。

> 板子规格：**ARM Cortex-A7 + 64MB DDR**。所有设计——惰性加载、原子位 fast-path、
> 文件流式下载、单 ALSA 实例——都是为这 64MB 内存让路的，下面架构里会反复提到。

---

## 1. 功能特性

- **双路摄像头**：cam0 (640×480 NV21，显示 + 编码)、cam1 (224×224 RGB888，AI 推理专用)
- **直播**：fMP4 over WebSocket，原生 `MediaSource` 可播；HTTPS/WSS 自签名证书
- **录像**：常驻按天分目录滚动落盘（单片时长可配置，默认 600s），双兜底清理（保留天数 + 总容量）
- **回放**：HTTP `Range` / 206 流式分片下载 + **`.idx` sidecar 精准跳转**（小到几 KB 的伴生 JSON
  把每个 fMP4 fragment 的 byte offset / tfdt 时间戳列了出来，前端拖动进度条时只下载
  目标 fragment 对应字节区间，毫秒级落点）
- **AI 抓拍**：YOLOv2 awnn person 模型（NPU），命中后写 JPEG 进相册并去重
- **移动侦测抓拍 (VMD)**：纯 CPU 帧间差分（NV21 Y 平面下采样到 80×60），与 AI 平行的轻量级触发器，
  无模型依赖、夜间无误检；命中后直接调用 `Snapshot::TakeOne`
- **音频**：板载 ALSA 麦克风 → AAC 编码进入 fMP4；浏览器 → OPUS → ALSA 单向对讲
- **OSD**：IP / 时间 / AI 框，叠加在 NV21 的 Y 平面上，零开销
- **运行时配置**：`config.json` 持久化 + `/api/config` 实时下发；模块在每个决策点
  pull-on-demand 读 snapshot，**改完即时生效，无需重启、无需订阅回调**

---

## 2. 项目结构

```
camera/
├── main/
│   ├── CMakeLists.txt
│   ├── dep/                       # 预编译依赖（FFmpeg / x264 / opus / OpenSSL / ALSA / 编解码）
│   └── src/
│       ├── main.cpp               # 进程入口；管理 cam0 / cam1 / vo / Terminal 生命周期
│       ├── terminal/              # 顶层调度器（聚合所有子模块、注册 HTTP API）
│       ├── h264Enc/               # 硬件 H264 编码（v4l2 / cedar）
│       ├── aacEnc/                # AAC 编码（FFmpeg libfdk_aac/aac）
│       ├── fmp4Muxer/             # ISO BMFF（init seg + moof/mdat 分片）
│       ├── liveHub/               # fMP4 fragment 的 pub/sub 中枢
│       ├── recorder/              # 滚动录像 + RecordCleaner 双兜底清理 + .idx 精准索引
│       ├── snapshot/              # JPEG 抓拍 + 相册（原子位 fast-path）
│       ├── personDetector/        # YOLOv2 person_int8 人形识别（cam1 注入）
│       ├── motionDetector/        # 帧间差分移动侦测（cam0 NV21，与 AI 平行的拍照触发器）
│       ├── httpServer/            # 多线程 HTTP/HTTPS（Range/206 流式）
│       ├── websocketServer/       # WS/WSS 服务（直播 / 回放 / 对讲）
│       ├── tlsContext/            # OpenSSL 上下文 + 自签名证书加载
│       ├── talkPlayer/            # OPUS → ALSA 单向对讲播放器
│       ├── appConfig/             # 单例配置 + JSON 持久化（pull-on-demand 读取）
│       └── utilTools/             # 日志、SPS 解析、shell 调试工具
├── person/                        # YOLOv2 person_int8 模型（推到 /root/models/）
├── web/                           # 前端资源（index.html / favicon.ico / css/js）
│   ├── index.html                 # 单页前端（直播 + 回放 + 对讲 + 配置）
│   └── favicon.ico
├── prompt/                        # 提示音资源（POST /api/prompt 播放）
│   └── door.wav
├── cert/                          # TLS 自签名证书（server.crt / server.key）
├── sync.sh                        # Mac → 编译机 → 板子 自动化部署脚本
├── scppush.sh                     # 备用：直接 scp 推产物到板子
├── debugcore.sh                   # 拉取板上 core 文件 + 交叉 gdb 调试
├── mem_watchdog.sh                # 内存看门狗（VmData>80MB 自动重启，应对闭源库慢泄漏）
├── project.py                     # libmaix 工程编译入口
└── README.md
```

---

## 3. 端口与服务

| 协议 | 端口 | 用途 |
|---|---|---|
| HTTP  | 8080 | 静态资源（`web/index.html`）+ REST API + 录像/相册下载 |
| HTTPS | 8443 | 同上，TLS（自签名证书） |
| WS  (`/ws/live`) | 8081 | fMP4 直播推流（init segment + 持续 fragment） |
| WS  (`/ws/audio`) | 8081 | **纯音频直播**（ADTS AAC 帧，省带宽） |
| WS  (`/ws/talk`) | 8081 | 浏览器→板子 OPUS 二进制对讲帧 |
| WS  (`/ws/playback`) | 8082 | 旧版回放协议（保留兼容） |
| WSS | 8444 / 8445 | live(含 `/ws/audio`) / playback 的 TLS 镜像 |

> HTTPS 仅在 `<exeDir>/cert/server.crt`、`server.key` 都存在时启用；
> 不存在时自动降级为明文，但前端对讲按钮在 `http://` 下会因浏览器的麦克权限策略隐藏。

---

## 4. 架构与数据流

```
            ┌─────────────────────────  M2dock (V831, 64MB)  ─────────────────────────┐
            │                                                                          │
   cam0 (640×480 NV21) ─┐                                                              │
            │           │                                                              │
            │           ▼  零拷贝采集                                                   │
            │     libmaix_vo (240×240 显示)                                           │
            │           │                                                              │
            │           ▼ Y 平面叠加 IP / 时间 / AI 框                                │
            │     C_Terminal::InputNv21                                                │
            │           │                                                              │
            │           ├──→ C_Snapshot      (atomic-flag fast-path)                   │
            │           ├──→ C_MotionDetector (帧间差分 → 命中 → C_Snapshot::TakeOne)  │
            │           ▼                                                              │
            │      C_H264Enc (硬件 VBR, GOP=30)  ──┐                                   │
            │                                      │                                   │
   ALSA mic ─→ C_AacEnc ──────────────────────────┤                                   │
            │                                      ▼                                   │
            │                              C_Fmp4Muxer                                 │
            │                                      │ ftyp+moov / moof+mdat            │
            │                                      ▼                                   │
            │                              C_LiveHub  (pub/sub)                       │
            │                                  │       │                              │
            │                                  ▼       ▼                              │
            │                          WebSocket    Recorder                          │
            │                          (live=8081/  (按天分目录，                     │
            │                           wss=8444)    片长可配置 record_segment_s，    │
            │                                        默认 600s，tfdt 重写 + .idx)     │
            │                                                                          │
   cam1 (224×224 RGB888) ─→ C_PersonDetector ─→ 命中 → C_Snapshot::TakeOne          │
            │       (NPU YOLOv2，1~10Hz，惰性载入 12MB 模型)                          │
            │                                                                          │
   browser /ws/talk OPUS ──→ C_TalkPlayer ──→ ALSA speaker                            │
            │                                                                          │
            │   HTTP/HTTPS ──→ httpServer (8080/8443) ──→ /api/* + Range 206         │
            └──────────────────────────────────────────────────────────────────────────┘
```

### 关键约束：V831 ISP 双 cam 创建顺序

V831 的 ISP 通道对 cam0 / cam1 创建顺序非常敏感——必须**先 `cam0->create + start_capture`，
再 `cam1->create + start_capture`，并且中间不能穿插 vo / Terminal / TLS / HTTP API 等
任何模块构造**，否则 cam0 输出会出现绿屏。

为此 [main.cpp](file:///Users/bytedance/work/libmaix/examples/camera/main/src/main.cpp)
在最前面把两路 cam 紧挨创建好之后，才把 `cam1` 通过构造参数注入 Terminal → PersonDetector。
PersonDetector 只"使用"该 cam，不 own，main 负责 destroy。

---

## 5. HTTP API 速查

| Method | Path | 说明 |
|---|---|---|
| GET | `/`、`/index.html` | 单页前端（来自 `<exeDir>/web/`） |
| GET | `/api/record/status`   | `{recording, file, bytes, root}` |
| GET | `/api/record/days`     | `["20260523", ...]` 倒序 |
| GET | `/api/record/segments?date=YYYYMMDD` | 当天分片 `[{name,size,hms,sec}]` |
| GET | `/record/<YYYYMMDD>/<name>.mp4` | **流式下载/inline 播放，支持 `Range` / 206** |
| GET | `/record/<YYYYMMDD>/<name>.mp4.idx` | **精准跳转伴生索引（JSON，几 KB）**：列出 init 段大小 + 每个 fragment 的 byte offset / size / tfdt，前端按需拉取目标 fragment 字节区间 |
| GET | `/api/photo/latest`    | 获取最新一张抓拍照片元信息 |
| GET | `/api/photo/list`      | 抓拍相册列表 |
| GET | `/photo/<name>.jpg`    | 单张抓拍下载 |
| POST| `/api/photo/delete`    | body `{names:[...]}` 或 `{name:"x.jpg"}`：批量/单张删除抓拍 |
| POST| `/api/snapshot`        | 立即拍一张 |
| POST| `/api/prompt`          | body `{name:"door"}`：同步播放 `<exeDir>/prompt/<name>.wav`（详见 §5.3） |
| GET | `/api/config`          | 当前 AppConfig snapshot（JSON） |
| POST| `/api/config` (form/json) | 局部更新 + 落盘；下个决策点（每帧 / 每片 / 每次扫描）即时生效 |

录像/相册下载走 `httpServer` 的 `ApiResponse::filePath` 流式分支，**不会**把整个文件读到
内存里——这是在 64MB 板子上的硬性约束。

### 5.1 回放精准跳转：`.idx` sidecar + HTTP Range

老的"整文件 fmp4"回放在 MSE 端只能从头解析才能得到帧位置，拖动进度条会
重新拉一大段字节、整页闪烁。本项目用 `.idx` 伴生 JSON 让前端"看一眼索引就知道
目标 fragment 在文件里的字节区间"，从而做到毫秒级跳转。

**录制端**：[recorder.cpp](file:///Users/bytedance/work/libmaix/examples/camera/main/src/recorder/recorder.cpp)
每写一个 mp4 同时维护一份 `<name>.mp4.idx`：

```json
{
  "v": 1, "ts": 90000, "init": 1248,    // init = ftyp+moov 字节数
  "frags": [ [0, 1248, 12345], ... ]    // 每项 [tfdt(90kHz), byte_offset, size]
}
```

每片 mp4 的 tfdt 时基落盘时由 `RewriteTfdtInPlace_locked` 重写为从 0 开始，
前端做时间→fragment 映射只需一次 lower_bound。`RecordCleaner` 删 mp4 时同步
`unlink` 同名 `.idx`。

**回放端**（[web/index.html](file:///Users/bytedance/work/libmaix/examples/camera/web/index.html)）：首次 GET `.idx`（几 KB），之后每次 seek：

1. 目标仍在 `SourceBuffer.buffered` 内 → 直接 `currentTime = t`，不发请求
2. 否则按 `.idx` 定位目标 fragment 的 `[off, off+size)`，单次 `Range` 拉回 append
3. 全程不重建 `<video>` 元素，无视口闪烁；`sb.mode='segments'` 按 tfdt 自动归位，
   向前/向后拖动都能毫秒级落点

**`mediaSource.duration` 即时锁定**：init seg append 完后立刻按 `.idx` 估算总时长
（`tfdt(last)/ts + 末段时长`）写入 `duration`，slider 立即可拖；idx 快路径不调
`endOfStream()`，让 MediaSource 全程保持 `open`。

服务端 [httpServer](file:///Users/bytedance/work/libmaix/examples/camera/main/src/httpServer)
对所有 `filePath` 响应解析 `Range:` 头返回 206，保持 64KB 滚动缓冲。
无 `.idx` 时前端自动回退到"整文件下载"老分支，可平滑铺设到既存录像目录上。

### 5.2 仅音频直播：`/ws/audio`（省带宽方案）

完整 fMP4 直播实际下行约 **1~2 Mbps**。当用户只想"听"现场（锁屏后台监听、
弱网、流量计费场景）时，服务端额外暴露一条独立 WebSocket 路径 `/ws/audio`，
**只推 ADTS AAC**，下行带宽降到 **~64 kbps**。

**服务端**（[websocketServer.cpp](file:///Users/bytedance/work/libmaix/examples/camera/main/src/websocketServer/websocketServer.cpp)）：
复用同一个 `C_WebSocketServer`（8081/8444），视频路径 `BroadcastBinary` 跳过
`/ws/audio` 客户端，新增 `BroadcastAudioBinary` 只向 `/ws/audio` 发包。帧来源在
[terminal.cpp::OnOutputAac](file:///Users/bytedance/work/libmaix/examples/camera/main/src/terminal/terminal.cpp)
——每帧 raw AAC 加 7 字节 ADTS 头后广播，**不依赖 muxer / 首个 IDR**，
视频通路尚未就绪时也能听到声音。

**浏览器端**（[web/index.html](file:///Users/bytedance/work/libmaix/examples/camera/web/index.html) 顶栏 `🎧 仅音频` 按钮）：
`stopLive()` 释放 MSE 后连 `/ws/audio`，用 `AudioContext.decodeAudioData` 解 ADTS，
单调递增的 `audioPlayHead` 调度播放，落后超 1.2s 自动追帧，排队 >12 帧丢一帧防抖动。
Safari/iOS 需用户手势触发 `audioCtx.resume()`。

| 维度 | `/ws/live` (fmp4) | `/ws/audio` (ADTS AAC) |
|---|---|---|
| 下行带宽 | ~1-2 Mbps | ~64 kbps |
| 解码栈 | MSE + `<video>` | Web Audio API |
| 起播延迟 | 等首个 IDR | 立即（ALSA 出帧即广播） |
| 互斥 | 进入仅音频时自动 `stopLive()` | 与视频直播互斥 |

> **单一编码源**：板上始终只跑一份 H264 + 一份 AAC 编码，无论几个客户端订阅，
> 64MB 板子才扛得住。

---

### 5.3 板上提示音播放：`POST /api/prompt`

web 端点一下"放门口"按钮，开发板扬声器立即播一段固定提示音（外卖配送提示、
欢迎语、告警语等）。链路：`POST /api/prompt {name:"door"}` →
[terminal.cpp](file:///Users/bytedance/work/libmaix/examples/camera/main/src/terminal/terminal.cpp)
校验白名单（`name` 仅 `[A-Za-z0-9_-]`）+ 拼路径 `<exeDir>/prompt/<name>.wav` →
[C_TalkPlayer::PlayWavSync](file:///Users/bytedance/work/libmaix/examples/camera/main/src/talkPlayer/talkPlayer.cpp)
同步播放（阻塞约等于 wav 时长）→ ALSA 内置扬声器。

**与对讲共存**：ALSA `default` 在 V831 上独占，TalkPlayer 既管对讲也管提示音——
`m_pcmMtx` 序列化 talk 解码线程与 prompt 的写入，`m_promptBusy` CAS 保证同时只播一段
（重叠请求返回 409）；talk 不在线时 prompt 临时打开 ALSA、播完归还。

**wav 部署**：源文件放 `examples/camera/prompt/*.wav`（已纳入 git），CMake
`copy_prompt_assets` 整目录扫到 `dist/prompt/`，随 `sync.sh push` 推到
`/root/maix_dist/prompt/`。格式必须是 `PCM / 48000Hz / 16-bit / mono`，否则返回 HTTP 500。
新增提示音先转码再放进 `prompt/`：

```bash
ffmpeg -i in.wav -ar 48000 -ac 1 -sample_fmt s16 examples/camera/prompt/welcome.wav
```

后端无需改动，但前端按钮目前硬编码 `door`，新增提示音需在
[web/index.html](file:///Users/bytedance/work/libmaix/examples/camera/web/index.html) 里加对应按钮。

**HTTP 响应码**：`200`（含 `duration_ms`）/ `400`（缺字段或非法 name）/
`404`（文件不存在）/ `409`（busy）/ `500`（格式不符或 ALSA 错误）。

---

## 6. 运行时配置 (`<exeDir>/config.json`)

由 [appConfig.h](file:///Users/bytedance/work/libmaix/examples/camera/main/src/appConfig/appConfig.h)
单例 `C_AppConfig` 管理：启动时 Load，每次 `ApplyPatch` 后 Save。

### 全局约定：pull-on-demand（单一真相源）

**各模块不持有 cfg 引用、不在自己内部缓存配置副本**，每次决策点
（每帧 / 每个 fragment / 每次 sweep / 每次 HTTP）调一次 `GetSnapshot()` 即可，
开销 < 1us，30Hz 帧回调里也完全够用：

| 决策点 | 频率 | 读到的字段 |
|---|---|---|
| `Recorder::OnLiveFragment` 落盘前 | 每个 fmp4 fragment | `record_segment_s` |
| `RecordCleaner::Sweep*` 周期扫描 | 1 次/小时 | `record_retain_days` / `record_max_bytes` |
| `MotionDetector::InputNv21` | 30Hz（disable 时仅 atomic load 立即返回） | 全部 `vmd_*` |
| `PersonDetector` 推理循环 | 1~10Hz | `ai_*`（false→true 由模块内 `m_modelLoaded` 状态机识别，触发 LoadModel） |
| `Snapshot::PruneOldestIfOver` 拍照后 | 1 次/拍 | `album_max_photos` / `photo_jpeg_qual` |
| `terminal` OSD 叠加 | 30Hz | `osd_show_*` |

**好处**：配置只存在 `m_snap` 一处，没有"模块副本与中心副本不同步"的可能；
零同步成本（不需要 atomic / Subscribe / cv 唤醒）；web 改完下一帧/下一片/下一次扫描就读到新值，**改完即时生效**。
项目早期版本曾设计过 Subscribe 回调机制，实际 0 订阅者，已作为 dead code 移除。

### 字段速查

| 字段 | 默认 | 说明 |
|---|---|---|
| `ai_enabled` | `false` | AI 推理总开关。**默认关闭**，用户在 web 上显式打开后才载 12MB 模型 |
| `ai_threshold` | `0.6` | YOLO confidence（0.30 ~ 0.90） |
| `ai_min_interval_s` | `5` | 同人去重秒数 |
| `ai_infer_fps` | `5` | NPU 推理帧率（1 ~ 10） |
| `record_segment_s` | `600` | 单片时长（秒），改完下一片即生效 |
| `record_retain_days` | `7` | 保留天数 |
| `record_max_bytes` | `16 GiB` | 总容量上限（含 `.mp4.idx` 伴生文件） |
| `vmd_enabled` | `false` | 移动侦测总开关。关闭时 InputNv21 只做一次 atomic load 立即返回 |
| `vmd_pixel_thresh` | `25` | 单像素差阈值（0 ~ 255 luma），越大越不敏感 |
| `vmd_area_ratio` | `0.02` | 变化像素占比阈值（0 ~ 1），0.02 即 2% 像素发生变化才认定为运动 |
| `vmd_min_interval_s` | `2` | 同一时段最短重复拍照间隔（秒） |
| `vmd_check_fps` | `5` | 每秒做几次差分判定（1 ~ 30），越高响应越快但 CPU 越费 |
| `album_max_photos` | `1000` | 相册超限按 mtime 删最老 |
| `photo_jpeg_qual` | `88` | JPEG 编码质量（50 ~ 95） |
| `mic_filter_mode` | `0` | 麦克风滤波：0=关闭 / 1=均衡 / 2=激进 / 3=精细 / 4=极激进 / 5=均衡+噪声门 |
| `osd_show_ip` / `osd_show_time` / `osd_show_ai_box` | `true` | OSD 显示开关 |

仅以下环境变量保留为调试旁路：`AAC_DUMP_PATH`（导出原始 AAC 流到 `/tmp/test.aac`）。
早期 `RECORD_SEGMENT_SEC` / `RECORD_RETAIN_DAYS` / `RECORD_MAX_BYTES` 已统一收敛到
`config.json` 字段，env 入口已移除。

> **不引第三方 JSON 库**：`appConfig` 自带极简 parser（只支持 `"key": <number|bool|string>`
> 的扁平对象），避免引 nlohmann 多 +40KB 二进制。

---

## 7. 编码参数

| 参数 | 值 |
|---|---|
| H264 Profile / Level | Main / 3.1 |
| H264 码率控制 / GOP / FPS | VBR / 30 / 30 |
| H264 QP 范围 | 5 ~ 40 |
| AAC 采样率 / 通道 / 码率 | 48 kHz / mono / 64 kbps |
| OPUS（下行对讲） | 浏览器 `MediaRecorder` 默认（WebM 容器内 48 kHz mono）|

---

## 8. 编译与部署

工程通过本地 → 编译机 → 开发板 三段式分发：

```
Mac (本地)              ──rsync──>  编译机                      ──scp──>  M2dock 开发板
~/work/libmaix          ─────────>   /root/work/libmaix          ────────>  /root/maix_dist
                                     python3 project.py build              start_app.sh
```

> 编译机 / 开发板的实际地址在 [sync.sh](file:///Users/bytedance/work/libmaix/examples/camera/sync.sh)
> 顶部 `BUILD_HOST` / `DEVICE_HOST` 配置（支持 IPv4 / IPv6 字面量）。

### 常用命令（[`sync.sh`](file:///Users/bytedance/work/libmaix/examples/camera/sync.sh)）

```bash
./sync.sh           # 仅同步源码到编译机
./sync.sh build     # 同步 + 在编译机执行 python3 project.py build
./sync.sh push      # 同步 + 编译 + scp 到板子 + 杀旧进程 + 拉新进程
./sync.sh run       # 仅推产物到板子并启动（不重新编译）
./sync.sh log       # tail 板子上 camera 的运行日志
./sync.sh clean     # 清理编译机上的 build/dist
```

> sync.sh 强制带上 `HostKeyAlgorithms=+ssh-rsa` 等老算法白名单，因为 V831 板上的
> dropbear/openssh 通常只支持 SHA-1 系算法，新版 OpenSSH (>=8.7) 默认禁用。

### 板上运行约定

- 可执行文件：`/root/maix_dist/camera`
- 依赖动态库：`/root/maix_dist/lib/`（启动脚本会 `LD_LIBRARY_PATH` 注入）
- 录像目录：`<exeDir>/record/<YYYYMMDD>/<HHMMSS>.mp4`
- 抓拍目录：`<exeDir>/snapshot/`
- 模型目录：`/root/models/person_int8.{bin,param}`（缺失时 AI 模块不致命）
- 证书目录：`<exeDir>/cert/server.{crt,key}`（缺失时降级 HTTP/WS 明文）
- 前端文件：`<exeDir>/web/index.html`（CMake 把 `web/` 整目录复制过去）

---

## 9. 内存优化锦囊（64MB 板上的取舍）

为了让所有模块 + FFmpeg + OpenSSL + OpenCV 一并跑在 64MB 里，做了以下针对性设计，
后续改动请保持这些约束：

1. **PersonDetector 模型惰性加载**：`ai_enabled=false` 时不加载 12MB 模型；
   推理循环每次 pull `ai_enabled`，由模块内 `m_modelLoaded` 状态机识别 false→true
   边沿后才 `LoadModel`，反之卸载——pull-on-demand 的典型边沿事件用法。
2. **MotionDetector 关闭即零开销**：[motionDetector.cpp:InputNv21](file:///Users/bytedance/work/libmaix/examples/camera/main/src/motionDetector/motionDetector.cpp)
   `vmd_enabled=false` 时一次 `m_running` atomic load 立即返回；启用时每帧只对
   80×60 = 4800 字节做差分（< 50us），触发拍照走独立后台线程，不阻塞 30Hz 主循环。
3. **Snapshot 原子位 fast-path**：[snapshot.cpp:OnNv21Frame](file:///Users/bytedance/work/libmaix/examples/camera/main/src/snapshot/snapshot.cpp)
   只在有抓拍请求挂起时才 memcpy 460KB，平时仅一次原子 load。
4. **录像 / 相册流式下载**：HTTP 只用 64KB 滚动缓冲 + `read+send`，绝不 `read_all`。
   `.idx` sidecar 把"前端为了 seek 必须扫整文件"的浪费也消除了。
5. **手写 JSON parser**：避免引入 nlohmann（+40KB 二进制）。
6. **fMP4 fragment pub/sub**：单写者多订阅者，订阅者各自维护 `deque<vector<uint8_t>>`，
   后端慢时主动丢老 fragment，不会把进程憋爆。
7. **TalkPlayer 单实例**：整机只开一个 ALSA PCM；构造时即 lazy init，第一个 talk
   客户端到来才真正 `snd_pcm_open`。
8. **AppConfig 无订阅者机制**：所有模块在自己的决策点 pull `GetSnapshot()`，不再
   维护 Subscribe 回调链表（dead code 已移除），少一份并发同步的复杂度。

---

## 10. 信号 / 优雅退出

捕获 `SIGINT / SIGTERM / SIGTSTP / SIGQUIT / SIGPIPE / SIGKILL` →
置 `g_apprun=false` → 主循环退出 → `delete pterminal`（先停 PersonDetector 线程，
再依次释放 vo / cam0 / cam1）→ `libmaix_module_deinit`。

> 由于 V831 cedar / VI / disp / snd 都是独占设备，
> 旧进程被杀后内核异步释放 fd 仍需 1~2 秒；`sync.sh push` 会主动 `pidof` 轮询 +
> `sleep 2` 等驱动收尾，否则新 camera 起来会卡在 `libmaix_camera_module_init` 几十秒。

---

## 11. 直播稳定性优化

fMP4 over WebSocket 直播在长时间运行和弱网环境下面临几类典型问题，
本项目做了三层针对性优化。

### 11.1 预缓冲起播（解决首帧卡顿）

**问题**：新客户端刚连上时，只收到 1 个 fragment 就调用 `video.play()`，
`readyState` 不足，画面卡在首帧不动或频繁缓冲。

**方案**：前端设置 `LIVE_START_BUFFER = 1.6s` 起播水位，
收满约 1.6 秒的 fragment 后再调用 `play()`，保证有足够的解码缓冲打底。

相关代码见 [web/index.html](file:///Users/bytedance/work/libmaix/examples/camera/web/index.html)
的 `liveStarted` 标志和 `pump()` 中的起播判定。

### 11.2 分级倍速追尾（解决延迟累积）

**问题**：直播播放速率 ≈ 实时速率，任何网络抖动都会让播放点逐渐落后于
直播源，延迟从几百毫秒累积到几秒甚至几十秒。

**方案**：三级追尾策略，平滑追上不突兀：

| 延迟区间 | 策略 | 说明 |
|---|---|---|
| < 1s | 正常 1.0x | 不追，保持观感流畅 |
| 1s ~ 2s | 1.1x 慢追 | 轻微加速，观众几乎无感 |
| 2s ~ 8s | 1.3x 中速追 | 明显加速但可接受 |
| > 8s | 硬 seek 到 live 边缘 | 直接跳转到最新关键帧（极端情况兜底） |

同时把 MSE 缓冲窗口放宽到 6s，给追尾留出操作空间。
页面可见时追尾更积极（阈值 1s），后台/不可见时放宽到 3s 避免频繁调整。

### 11.3 大 PTS 累积修复（方案 C：timestampOffset 归零）

**问题**：服务端 fMP4 的 PTS 来自 `C_TimeBase::NowUs()` 单调时钟，
板子长时间运行（50+ 小时）后 PTS 累积到 19 万秒。新客户端接入时
MSE 的解码基准时间轴错位（`currentTime` 从十几万秒开始），
导致首帧定格、进度条异常、`buffered` 范围巨大无法正常播放。

**方案**：前端在 append 首个 media fragment 前，解析 fMP4 moof/traf/tfdt box
拿到 `baseMediaDecodeTime`，然后设置 `SourceBuffer.timestampOffset = -(tfdt / 90000)`，
把时间轴拉回到 0 点附近。这样无论板子跑了多久，新客户端看到的都是
从 0 开始的正常时间轴。

关键实现：

- `parseFirstTfdt(arrayBuf)`：递归扫描 MP4 box，定位 `moof → traf → tfdt`，
  返回 64 位 baseMediaDecodeTime（90kHz 时基）
- 在 `pump()` 首次 append 前设置 `sb.timestampOffset`
- `VIDEO_TIMESCALE = 90000` 与服务端 fmp4Muxer 的时基一致
- 每次重连（`reconnectLive`）时复位 `tsOffsetSet` 标志，重新计算

> 为什么不在服务端重写 tfdt？服务端是单生产者多消费者架构，
> 重写 tfdt 需要为每个客户端单独维护一份 fragment 拷贝，内存和 CPU 开销
> 在 64MB 板子上不可接受。方案 C 把计算量全部转移到前端，
> 服务端零改动，是最经济的解法。

---

## 12. 运维工具

### 12.1 内存看门狗 (`mem_watchdog.sh`)

V831 板子上的闭源库（cedar / NPU / FFmpeg 等）存在约 **0.5 MB/h** 的慢泄漏，
连续运行几天后 VmData 会涨到 60MB+，触发 OOM 被杀。
为了在修复泄漏之前保证 7×24 稳定运行，配置了内存看门狗：

- **阈值**：VmData > 80 MB 时触发重启
- **检测间隔**：60 秒
- **日志**：每 5 分钟打一条 "mem ok" 心跳，异常时打印进程内存快照
- **日志上限**：2 万行自动截断
- **时间戳**：北京时间（UTC+8）

部署方式：随 `S02app` 开机自启（`start_app.sh` 拉起 camera 后，
sleep 30 秒再拉起看门狗）。脚本路径：`/root/maix_dist/mem_watchdog.sh`。

### 12.2 Core 调试 (`debugcore.sh`)

板子进程崩溃产生 core 文件后，用 `debugcore.sh` 一键拉取并启动交叉 gdb：

```bash
./debugcore.sh          # 拉取 core + run.log → dist/ → 启动 gdb
./debugcore.sh pull     # 只拉取文件，不启动 gdb
```

交叉工具链路径默认 `/opt/toolchain-sunxi-musl/`，
不在该路径时脚本会提示修改 `GDB_TOOLCHAIN` 变量。

---

## 13. 已知问题 / TODO

| 编号 | 问题 | 状态 |
|---|---|---|
| C1 | [main.cpp:251](file:///Users/bytedance/work/libmaix/examples/camera/main/src/main.cpp#L251) `if (x1<0)…; if (y1<0)…;` 同行 `-Wmisleading-indentation` 告警 | 待处理（仅告警，不影响运行） |
| B1 | TLS 握手在 accept 线程，影响多 HTTPS 客户端并发接入延迟 | 待处理 |
| A1 | 每个 HTTP 连接一个 8MB 栈的线程 | 待处理（可改线程池或缩栈到 256KB） |
| A2 | `InputRgb888` / `rgb888ToNv21` / `m_pNv12Buff` 死代码 | 待清理（仅被 main.cpp `#if 0` 旧采集路径引用） |
| A3 | `SimpleSHA1` / `SimpleBase64` 自实现握手摘要 | 待优化（OpenSSL 已链接，可换 `SHA1()`） |
| C2 | 启动 `sleep_for(8s)` 等 IP / NTP | 待优化（改循环检测 + 可中断） |

---

## 14. 依赖与许可

- **libmaix**：摄像头 / 显示 / VO / NPU 抽象（同仓库根 `components/`）
- **FFmpeg / x264 / opus**：AAC 编码 + OPUS 解码（`main/dep/ffmpeg/`）
- **OpenSSL 1.1**：TLS（HTTPS / WSS）+ 自签名证书
- **OpenCV**：OSD 文字 / JPEG 编码（已由 libmaix 引入）
- **ALSA**：麦克风采集 + 对讲扬声器播放
- **awnn**：V831 NPU 推理后端（YOLOv2 person_int8）
