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
- **直播**：fMP4 over WebSocket，支持 `MediaSource` / iOS `ManagedMediaSource`；
  HTTPS/WSS 自签名证书
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
│       ├── mqttReporter/          # IPv6 地址变化 → MQTT 上报（手写 QoS0 客户端）
│       ├── httpClient/             # 出站 HTTP/1.0 客户端（设备动作代理 POST，仅 http://）
│       ├── actionStore/            # 设备动作「按钮→URL」持久化（actions.json，可增删）
│       ├── logBroadcaster/         # 日志广播：CLOG_* → /ws/log 订阅者（有订阅者才推）
│       ├── sysInfoProvider/        # 系统资源采集（CPU/内存/VmData/磁盘/uptime，读 procfs）
│       ├── lightController/        # 外接补光灯 GPIO 控制（PH13，时段/声控/持续时长）
│       ├── appConfig/             # 单例配置 + JSON 持久化（pull-on-demand 读取）
│       └── utilTools/             # 日志、SPS 解析、shell 调试工具
├── person/                        # YOLOv2 person_int8 模型（推到 /root/models/）
├── web/                           # 前端资源（index.html / favicon.ico / css/js）
│   ├── index.html                 # 页面语义结构（直播 + 回放 + 相册 + 配置）
│   ├── style.css                  # 响应式监控台视觉与移动端布局
│   ├── app.js                     # MSE/WS 状态机、回放、相册与设置交互
│   ├── favicon.svg                 # 摄像头光圈图标（矢量，浏览器标签页首选）
│   └── favicon.ico                 # 同款图标位图兜底（16/32/48，老浏览器）
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
| HTTP  | 80 | 静态资源（`web/index.html`）+ REST API + 录像/相册下载 |
| HTTPS | 443 | 同上，TLS（自签名证书） |
| WS  (`/ws/live`) | 8081 | fMP4 直播推流（init segment + 持续 fragment） |
| WS  (`/ws/audio`) | 8081 | **纯音频直播**（ADTS AAC 帧，省带宽） |
| WS  (`/ws/talk`) | 8081 | 浏览器→板子 OPUS 二进制对讲帧 |
| WS  (`/ws/log`) | 8081 | **设备运行日志实时推送**（CLOG_* 输出，UTF-8 文本；仅有订阅者时才推） |
| WS  (`/ws/playback`) | 8082 | 旧版回放协议（保留兼容） |
| WSS | 8444 / 8445 | live(含 `/ws/audio`、`/ws/log`) / playback 的 TLS 镜像 |

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
            │   HTTP/HTTPS ──→ httpServer (80/443)    ──→ /api/* + Range 206         │
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
| GET | `/api/netinfo`         | 设备真实网卡地址 `{ipv4, ipv6}`：IPv4 枚举网卡取首个非回环地址；IPv6 取监测网卡（`mqtt_iface`，默认 wlan0）全局单播地址（排除 `fe80::`/`::1`，取不到为空串）。实况页「设备状态」卡片展示 |
| GET | `/api/sysinfo`         | 设备系统资源快照 `{cpu, load, mem, proc, disk, uptime, proc_uptime}`：读 procfs/statvfs。CPU% 为两次请求间 `/proc/stat` 差值（首次 `cpu.valid=false`）；`proc` 含本进程 VmData 与 mem_watchdog 阈值；`uptime` 为整机开机时长、`proc_uptime` 为 camera 进程运行时长（详见 §5.6）。实况页「设备状态」5s 轮询 |
| GET | `/api/actions`         | 设备动作代理列表 `{ok, actions:[{id,name,url}]}`：web 上可增删的「按钮→URL」映射（详见 §5.4） |
| POST| `/api/actions`         | body `{name, url}`：新增一个动作。`url` 仅接受 `http://`；成功 201 返回 `{ok,action}`，数量超上限（32）返回 409，其它校验失败 400 |
| POST| `/api/actions/update`  | body `{id, name, url}`：按 id 就地修改按钮名/URL，校验规则同新增；成功 `{ok,action}`，id 不存在 404，校验失败 400 |
| POST| `/api/actions/delete`  | body `{id}`：删除指定动作，不存在返回 404 |
| POST| `/api/actions/invoke`  | body `{id}`：由**后端出站** POST 到该动作的 `url`（避免浏览器跨域/混合内容限制）。成功返回 `{ok,status,name}`；出站链路失败返回 502 带 `err` |

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

**回放端**（[web/app.js](file:///Users/bytedance/work/libmaix/examples/camera/web/app.js)）：首次 GET `.idx`（几 KB），之后每次 seek：

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

当前监控台未暴露独立的“仅音频”入口；定制客户端可直接连接 `/ws/audio`，
用 Web Audio 或原生 AAC 解码器消费 ADTS 帧。Safari/iOS 仍需用户手势触发
`AudioContext.resume()`。

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

### 5.4 设备动作代理：web 可增删的「按钮→URL」出站 POST

需要点一下 web 按钮就让摄像头去局域网里的某个设备（门锁 / 灯控 / 面板等）发一条
指令时，直接用浏览器 `fetch` 往目标设备发会撞上两堵墙：**跨域（CORS）** 和
**混合内容**（HTTPS 页面禁止请求 `http://` 目标）。本项目改由**后端代理出站**：
浏览器只调本机同源的 `/api/actions/invoke`，真正的 POST 由开发板发出。

- **配置持久化**：动作列表存 `<exeDir>/actions.json`（数组 `[{id,name,url}]`），
  由 [actionStore.cpp](file:///Users/bytedance/work/libmaix/examples/camera/main/src/actionStore/actionStore.cpp)
  加锁读写。单独一份文件而不塞进 `config.json`，是因为 `appConfig` 的解析器只支持
  扁平标量，撑不了数组/对象。上限 32 条，`name`≤32 字节、`url`≤512 字节，均过滤控制字符。
- **出站客户端**：[httpClient.cpp](file:///Users/bytedance/work/libmaix/examples/camera/main/src/httpClient/httpClient.cpp)
  是纯 libc socket 实现的极简 HTTP/1.0 客户端：`getaddrinfo` 解析（支持
  `http://host[:port][/path]` 与 `[IPv6]:port` 字面量）→ 非阻塞 connect + `select`
  超时（默认 5s）→ 发 `POST` + `Connection: close` → 只读状态行取状态码。**仅支持
  `http://`**（板上不做 HTTPS 客户端）；出站 POST 阻塞跑在 HTTP 客户端线程，不影响
  采集/编码/直播线程。
- **前端**：实况页「设备动作」卡片列出按钮，点击即 `invoke`；卡片内 `<details>`
  折叠出「名称 + URL」表单，支持新增、**编辑**（点列表行「编辑」把该项填回表单、
  按钮切「保存」并高亮当前行，可「取消」）与删除。列表为空时整卡隐藏。初始
  `actions.json` 为空，开门/开灯/关灯等按钮由使用者自行在 web 上添加。

**HTTP 响应码**：`GET /api/actions` → `200`；`POST /api/actions` → `201` / `400`
（校验失败）/ `409`（超上限）；`POST /api/actions/update` → `200` / `400`（校验失败）
/ `404`（id 不存在）；`POST /api/actions/delete` → `200` / `404`；
`POST /api/actions/invoke` → `200`（含目标 `status`）/ `404`（id 不存在）/
`502`（出站链路失败，带 `err`）。

### 5.5 设备运行日志实时查看：`/ws/log`

实况页「设备运行日志」折叠框展开后，浏览器会订阅 `/ws/log`，把开发板上
`CLOG_*` / `NLOG_*` 的输出实时推到网页（免 ssh 上板 `tail -f run.log`）。设计要点：

- **解耦的日志出口**：所有日志最终汇入
  [C_LogAdapt::LogInner](file:///Users/bytedance/work/libmaix/examples/camera/main/src/utilTools/logAdapt.cpp)，
  它照常写 `stdout` + `run.log`，末尾多一步"分叉给 sink"。logAdapt 只认识抽象接口
  `ILogSink`，不依赖 WebSocket，避免底层日志模块反向依赖上层网络模块。
- **有界缓冲 + 独立推送线程**：
  [logBroadcaster.cpp](file:///Users/bytedance/work/libmaix/examples/camera/main/src/logBroadcaster/logBroadcaster.cpp)
  实现 `ILogSink`。业务线程（相机 / 编码 / 网络）只把日志行**入有界队列**（上限 200 行，
  超限丢最早）；真正的广播由一条独立线程在锁外完成——弱网网页**绝不反压**采集/编码。
- **零空载开销**：无 `/ws/log` 订阅者时（原子计数为 0），`OnLogLine` 第一行即 return，
  不构造字符串、不入队。订阅者数由 terminal 在 `/ws/log` 握手/断开时维护。
- **自激防护**：推送线程整个生命周期 `SetThreadLogSuppressed(true)`，因此广播路径
  （含 WS 出错打的日志）产生的日志不会再回灌队列，杜绝无限自激与死锁。
- **传输与呈现**：后端用 WS **binary 帧承载 UTF-8** 文本（复用直播已有的有界发送队列，
  不改热路径）；前端 `TextDecoder` 解码后按行渲染，**最多 100 行**、超限删最早，并按
  级别染色（ERR/FLT 红、WRN 黄、INF 灰）。折叠框收起或切走实况页即断开连接。

---

### 5.6 设备系统资源监控：`GET /api/sysinfo`

实况页「设备状态」卡片除网卡/录像外，还每 5 秒轮询 `/api/sysinfo` 展示 CPU、
内存、进程内存、存储、运行时长，每项带一条按水位染色的进度条
（<70% 青、70~90% 黄、>90% 红）。采集实现在
[sysInfoProvider.cpp](file:///Users/bytedance/work/libmaix/examples/camera/main/src/sysInfoProvider/sysInfoProvider.cpp)，
全部只读、无副作用：

- **CPU 使用率**：读两次 `/proc/stat` 的 cpu 汇总行求 busy/total 差值。因此需要跨请求
  保留上次采样（`C_SysInfoProvider` 持有 `m_lastCpu`，加锁保护），**首次请求
  `cpu.valid=false`**（无历史样本），第二次起才有值。另附 `/proc/loadavg` 负载。
- **系统内存**：`/proc/meminfo` 的 `MemTotal` 与 `MemAvailable`（老内核回退 `MemFree`）。
- **进程内存**：`/proc/self/status` 的 `VmRSS` / `VmData`。**`VmData` 正是
  [mem_watchdog.sh](file:///Users/bytedance/work/libmaix/examples/camera/mem_watchdog.sh)
  监控的指标**（闭源库慢泄漏体现在此，到 80MB=81920KB 阈值就重启）；响应里回显该阈值，
  web 上以「进程内存 / 重启阈值」进度条直观显示"离看门狗重启还有多远"。
- **存储**：对录像目录做 `statvfs`（eMMC 用户分区，关心"还能录多久"）。
- **运行时长**：分两项——
  - **系统运行**（`uptime`）：`/proc/uptime`，整机自开机起的秒数。
  - **进程运行**（`proc_uptime`）：`系统 uptime − /proc/self/stat` 第 22 字段 `starttime`/`sysconf(_SC_CLK_TCK)`。
    这是 camera 进程本身已运行多久，**因 mem_watchdog 到阈值会重启进程，该值能直观反映
    "距上次（看门狗/手动）重启多久"**，比系统 uptime 更贴合运维观察。解析时注意 `/proc/self/stat`
    第 2 字段 comm 可能含空格/括号，须从最后一个 `)` 之后再按空格切字段。

每一项都带 `valid` 标志：某来源在当前平台不可读时（如非 Linux 环境无 procfs）
返回 `valid=false`，前端显示「不可用」而非崩溃或误显 0。

### 5.7 外接补光灯：`lightController/`（GPIO PH13）

监控摄像头本身无补光，外接一路 LED 灯板（`VCC/GND/DO` 三针），由
[lightController.cpp](file:///Users/bytedance/work/libmaix/examples/camera/main/src/lightController/lightController.cpp)
的常驻线程按配置每 ~500ms 评估"此刻是否该亮"，通过 sysfs 写 GPIO 电平。

**接线（GPIO 只做控制信号，灯单独供电）**：

```
M2Dock                         LED 灯板
────────                       ────────
独立 5V+           ──────────> VCC       （大功率灯不要直接吃开发板 5V）
GND（开发板+电源共地）──────────> GND
PH13 / GPIO237 ──470Ω~1kΩ────> DO        （3.3V 控制信号；勿把 5V 接回 PH13）
```

- **控制脚**：默认 `PH13 = 237`（`7×32+13`）。板上主 PIO 控制器 `gpiochip0` base=0/ngpio=288，
  已实测 `echo 237 > /sys/class/gpio/export` 可用。不用 `PH14=238`，因其与板载 State LED 复用。
- **GPIO 访问**：sysfs（`export` → `direction=out`（初始 low，避免上电误亮）→ 持久打开 `value`
  fd，切换只写一字节）。Stop 时先灭灯、关 fd、`unexport`。编号由 `light_gpio` 配置，越界回退 237。
- **触发逻辑**（现拉 AppConfig，改完即时生效）：不在时段或未启用→灭；时段内 `light_mode=0` 常亮；
  `light_mode=1` 声控——麦克风响度 ≥ `light_sound_thresh` 时把点亮截止时刻推到 `now+light_hold_s`，
  期间再有声音会续期。时段 `[light_start_hour, light_end_hour)` 按北京时间（UTC+8）解释，支持跨零点（如 18→6）。
- **响度来源**：复用 AAC 采集线程——每 20ms 的原始 S16 PCM 顺带算一次 RMS（低响度放大后封顶 0~100），
  通过一个全局原子量发布，声控读它，无额外线程/设备占用（V831 ALSA `default` 独占）。
- **配置项**：`light_enabled`（默认关）、`light_mode`、`light_start_hour`/`light_end_hour`、
  `light_sound_thresh`（0~100）、`light_hold_s`、`light_gpio`、`light_active_low`（低电平点亮灯板勾选）。
  v3 在 v2 响度上再放大 5 倍并封顶 100；旧配置会一次性迁移（v2 的 7→v3 的 35）
  并写入版本标记，避免重复放大。

> ⚠️ 时段明确按北京时间（UTC+8）判定；仍依赖板端 epoch 正确，若无 NTP/RTC 校准，"晚上"的判定会不准。
> 大功率灯务必独立供电 + 与开发板共地，灯板附近并联 100nF + 100~470µF 电容抑制开灯瞬间对摄像头电源的干扰。

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
| `mqtt_enabled` | `false` | IPv6 地址 MQTT 上报总开关。**默认关闭**，关闭时后台线程空转、零网络开销 |
| `mqtt_broker_host` | `broker.emqx.io` | MQTT broker 地址（域名 / IPv4 / IPv6 均可） |
| `mqtt_broker_port` | `1883` | broker 端口（明文 MQTT，1 ~ 65535） |
| `mqtt_topic` | `cam/ipv6` | 发布主题；payload 为纯 IPv6 字符串 |
| `mqtt_client_id` | `v831cam` | MQTT client id（同 broker 下多设备需区分） |
| `mqtt_poll_sec` | `10` | IPv6 轮询周期（秒，2 ~ 3600） |
| `mqtt_iface` | `wlan0` | 监测的网卡名（取其全局 IPv6） |
| `mqtt_report_interval_s` | `3600` | 保活重报周期（秒）：地址没变也每隔此间隔重报一次（心跳）；`0`=关闭仅变化时报；非 0 时下限 60、上限 86400 |
| `mqtt_retain` | `true` | 是否让 broker 保留消息（retain）：新订阅者一连上即收到最后一次上报的地址 |

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
- 前端文件：`<exeDir>/web/{index.html,style.css,app.js}`（CMake 整目录复制）

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

捕获 `SIGINT / SIGTERM`（`SIGPIPE` 全局忽略）→ 置 `g_apprun=false` → 主循环退出 →
`delete pterminal`。`Terminal::Stop()` 先停止并 join HTTP/两个 WebSocket 的
accept/client 线程，再停止 AAC 生产线程，随后释放 muxer、录像、检测与音频资源；
最后 main 依次释放 vo / cam0 / cam1 并执行 `libmaix_module_deinit`。

AAC 与 WebSocket 都采用显式 `Start/Stop`：构造阶段只准备资源，等 `Terminal`
完整构造后才启动可能回调宿主的线程，避免构造期发布未完成的 `this`。

> 由于 V831 cedar / VI / disp / snd 都是独占设备，
> 旧进程被杀后内核异步释放 fd 仍需 1~2 秒；`sync.sh push` 会主动 `pidof` 轮询 +
> `sleep 2` 等驱动收尾，否则新 camera 起来会卡在 `libmaix_camera_module_init` 几十秒。

---

## 11. 直播稳定性优化

fMP4 over WebSocket 直播在长时间运行和弱网环境下面临几类典型问题，
本项目从时间轴、缓冲水位、发送背压和浏览器能力探测四个层面处理。

### 11.1 预缓冲起播（解决首帧卡顿）

**问题**：新客户端刚连上时，只收到 1 个 fragment 就调用 `video.play()`，
`readyState` 不足，画面卡在首帧不动或频繁缓冲。

**方案**：前端设置 `LIVE_START_BUFFER = 1.8s` 起播水位，起播点保持在
`buffered.end - 1s` 左右。fMP4 约每秒抵达一片，若只落后末端 0.1~0.4 秒，
播放头会在下一片到达前耗尽缓冲，反而形成周期性 `waiting`。

相关代码见 [web/app.js](file:///Users/bytedance/work/libmaix/examples/camera/web/app.js)
的 `liveStarted` 标志和 `pump()` 中的起播判定。

### 11.2 分级倍速追尾（解决延迟累积）

**问题**：直播播放速率 ≈ 实时速率，任何网络抖动都会让播放点逐渐落后于
直播源，延迟从几百毫秒累积到几秒甚至几十秒。

**方案**：三级追尾策略，平滑追上不突兀：

| 延迟区间 | 策略 | 说明 |
|---|---|---|
| ≤ 0.8s | 正常 1.0x | 停止追尾，避免耗尽前向缓冲 |
| 1.5s ~ 2.5s | 1.08x 慢追 | 平滑回到目标水位 |
| 2.5s ~ 8s | 1.2x 中速追 | 加快消化网络积压 |
| > 8s | seek 到 `end-1s` | 保留一片抗抖动水位 |

同时把 MSE 缓冲窗口放宽到 6s，给追尾留出操作空间。
页面隐藏或锁屏时主动断开直播，恢复可见后重建 MSE 并重新追到实时位置，
避免移动浏览器后台限速造成延迟和内存持续累积。

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
- `tfdt=0` 同样必须把 `tsOffsetSet` 置为真；否则第二片才设置负偏移，
  会被映射回 0 秒并与首片重叠，直接导致首帧后定格
- `VIDEO_TIMESCALE = 90000` 与服务端 fmp4Muxer 的时基一致
- 每次重连时复位 `tsOffsetSet` 标志，重新计算

> 为什么不在服务端重写 tfdt？服务端是单生产者多消费者架构，
> 重写 tfdt 需要为每个客户端单独维护一份 fragment 拷贝，内存和 CPU 开销
> 在 64MB 板子上不可接受。方案 C 把计算量全部转移到前端，
> 无需在服务端做逐客户端 tfdt 改写，是更经济的时间轴处理方式。

### 11.4 单连接发送隔离（解决弱网拖死采集）

握手线程先同步发完 init segment，再把连接置为 `WS_OPEN`，从协议顺序上保证
fragment 不会抢在 init 前面。后续直播数据只写入每客户端的有界队列
（最多 8 片 / 1 MiB），由该客户端线程串行执行 socket/SSL 写操作。
队列满时丢最老片段；muxer 只在 IDR 边界切片，所以新片可独立恢复。
慢客户端不再阻塞 H264 回调和相机帧释放。

所有客户端线程均由服务器持有并在 `Stop()` 时 join，不使用 detached thread。
HTTP 限制请求头 16 KiB、body 1 MiB、完整请求读取 10 秒，并最多同时处理 6 个客户端；
WebSocket 限制握手 16 KiB、客户端入站帧 64 KiB，直播/旧回放分别最多 6/2 个客户端。
长度检查使用减法形式避免 64 位 payload 长度回绕，超限或协议错误直接断开连接。

### 11.5 移动浏览器兼容与自恢复

- 从 init segment 的 `avcC` 动态生成实际 AVC codec 字符串，不再只信硬编码 profile
- 同时探测 `MediaSource`、iPhone Safari 的 `ManagedMediaSource` 和旧 WebKit 前缀
- iPhone 的 MMS 使用 `<source src="blob:...">` 绑定并禁用远程播放，普通 MSE 仍使用
  `video.src`
- 兼容 WebSocket 返回 `ArrayBuffer` 或 `Blob`
- 校验首包必须包含 `ftyp+moov`，异常片段丢弃并重新请求 IDR
- 连接关闭、8 秒无媒体片段或播放时间轴停滞 6 秒时指数退避重连
- 浏览器没有 MediaSource 时只禁用直播，相册、设置和原生录像回放仍可使用，
  不再因自动起播抛异常而中断整页脚本
- 旧录像若曾在 P 帧处切 fragment，WebKit 的首个 `buffered` 区间可能晚于 0 秒；
  回放会自动把播放头钳制到首个可解码时间，避免停在 `HAVE_METADATA`

#### iPhone / iPad 兼容范围

| 环境 | 直播 | `.idx + Range` 回放 | 说明 |
|---|---|---|---|
| iOS / iPadOS 17.1+ Safari | 支持 | 支持 | 使用 `ManagedMediaSource`，建议升级到当前系统版本 |
| iOS / iPadOS 17.0 及更早 | 不支持 | 有限回退 | 系统没有 MSE/MMS；直播会明确提示升级，回放仅尝试原生 MP4 |
| macOS Safari、Chrome、Edge | 支持 | 支持 | 使用标准 `MediaSource` |

iOS 上的 Chrome、Edge 等浏览器若使用系统 WebKit，能力边界与 Safari 相同。直播默认
静音以满足自动播放策略，开启声音或回放被系统拦截时需要用户点击播放器。通过 HTTPS/WSS
访问自签名证书服务时，必须先在设备上信任证书，否则 Safari 会阻止 WebSocket 连接。
旧 iOS 的原生 MP4 回退无法保证播放 `empty_moov` 录像，完整直播和精准回放以
iOS / iPadOS 17.1+ 为最低支持版本。

### 11.6 监控台 UI 与资源管理

- `index.html`、`style.css`、`app.js` 按结构、视觉和逻辑拆分，仍保持零框架依赖
- 桌面端左列为画面 + 直播控制/设备动作、右侧栏为紧凑「设备状态」列表，双列设置卡片；手机端改为底部导航和单列布局
- 切离实况或页面进入后台时释放 MSE/WebSocket，返回后自动恢复
- API 统一增加超时和错误返回；设置页提示未保存状态并校验数值
- 大录像下载走浏览器原生流，回放预拉只缓存一份不超过 8 MiB 的小文件，
  避免手机内存被完整 MP4 或多份预拉缓存占满

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

### 12.3 IPv6 地址 MQTT 上报 (`mqttReporter/`)

板子的公网 IPv6 由运营商动态下发、会不定期变化。外部要通过 IPv6 直连板子的
web 服务（80/443）就得知道当前地址。为此
[mqttReporter](file:///Users/bytedance/work/libmaix/examples/camera/main/src/mqttReporter)
常驻一个后台线程，**定时轮询 wlan0 的全局 IPv6，变化时用 MQTT 把新地址发布出去**。

**链路**：`getifaddrs` 取网卡全局 IPv6（排除 `fe80::` link-local 与 `::1`）→
与上次缓存比较 → 变化则用手写 MQTT 客户端
[mqttClient](file:///Users/bytedance/work/libmaix/examples/camera/main/src/mqttReporter/mqttClient.h)
`connect → CONNECT → PUBLISH(QoS0) → DISCONNECT` 发布**纯 IPv6 字符串** payload。

**设计要点**：
- **手写 MQTT 客户端**：只实现 QoS0 发布，纯 libc socket（`getaddrinfo` 支持
  v4/v6 broker），无第三方库、二进制零膨胀，契合 64MB 板。
- **按需短连接**：检测到地址变化、或到达保活重报周期时才连 broker，publish 完立即断开，
  不维持长连接。
- **保活重报**：`mqtt_report_interval_s`（默认 1 小时）到点即使地址没变也重报一次，
  作为心跳让订阅端感知设备存活、新订阅者能及时拿到当前地址；设 `0` 则仅变化时报。
- **保留消息（retain）**：`mqtt_retain`（默认开）让 broker 存住最后一次地址，
  新订阅者一连上立即收到当前 IPv6，无需等下次上报；与保活重报配合，值几乎不会过期。
- **失败下轮重试**：publish 失败不更新缓存，下一轮轮询仍会重发。
- **默认关闭**：`mqtt_enabled=false` 时线程只空转睡眠、不采集不连网。
- **pull-on-demand**：broker / topic / 周期 / 网卡等每轮从 `AppConfig` 现取，
  web 改完下一轮即生效（配置字段见 §6）。

**验证**：订阅端 `mosquitto_sub -h broker.emqx.io -t cam/ipv6 -v`；
在板子上 `mqtt_enabled=true` 后，冷启动会立刻收到当前 IPv6，之后地址变化或每到保活周期都会推送。

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
