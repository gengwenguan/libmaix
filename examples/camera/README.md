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
│   └── src/
│       ├── main.cpp               # 进程入口；管理 cam0 / cam1 / vo / Terminal 生命周期
│       ├── terminal/              # 顶层调度器（聚合所有子模块、注册 HTTP API）
│       ├── h264Enc/               # 硬件 H264 编码（v4l2 / cedar）
│       ├── aacEnc/                # AAC 编码（FFmpeg libfdk_aac/aac）
│       ├── fmp4Muxer/             # ISO BMFF（init seg + moof/mdat 分片）
│       ├── liveHub/               # fMP4 fragment 的 pub/sub 中枢
│       ├── recorder/              # 滚动录像 + RecordCleaner 双兜底清理 + .idx 精准索引
│       ├── snapshot/              # JPEG 抓拍 + 相册（原子位 fast-path）
│       ├── personDetector/        # YOLOv2 awnn 人形识别（cam1 注入）
│       ├── motionDetector/        # 帧间差分移动侦测（cam0 NV21，与 AI 平行的拍照触发器）
│       ├── httpServer/            # 多线程 HTTP/HTTPS（Range/206 流式）
│       ├── websocketServer/       # WS/WSS 服务（直播 / 回放 / 对讲）
│       ├── tlsContext/            # OpenSSL 上下文 + 自签名证书加载
│       ├── talkPlayer/            # OPUS → ALSA 单向对讲播放器
│       ├── appConfig/             # 单例配置 + JSON 持久化（pull-on-demand 读取）
│       └── utilTools/             # 日志、SPS 解析、shell 调试工具
├── dep/                           # 预编译依赖（FFmpeg / x264 / opus / OpenSSL / ALSA / 编解码）
├── person/                        # YOLOv2 awnn 模型（推到 /root/models/）
├── web_player.html                # 单页前端（直播 + 回放 + 对讲 + 配置）
├── webcodecs_test.html            # WebCodecs 验证页
├── sync.sh / scppush.sh / buildpush.sh   # Mac → 编译机 → 板子 自动化脚本
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
| POST| `/api/snapshot`        | 立即拍一张 |
| POST| `/api/prompt`          | body `{name:"door"}`：同步播放 `<exeDir>/prompt/<name>.wav`（详见 §5.3） |
| GET | `/api/config`          | 当前 AppConfig snapshot（JSON） |
| POST| `/api/config` (form/json) | 局部更新 + 落盘；下个决策点（每帧 / 每片 / 每次扫描）即时生效 |

录像/相册下载走 `httpServer` 的 `ApiResponse::filePath` 流式分支，**不会**把整个文件读到
内存里——这是在 64MB 板子上的硬性约束。

### 5.1 回放精准跳转：`.idx` sidecar + HTTP Range

老的"整文件 fmp4"回放在 MSE 端只能从头解析才能得到帧位置，拖动进度条会
重新拉一大段字节、整页闪烁、卡顿明显。本项目用 `.idx` 伴生 JSON 让前端
"看一眼索引就知道目标 fragment 在文件里的字节区间"，从而做到毫秒级跳转：

**录制端**：[recorder.cpp](file:///Users/bytedance/work/libmaix/examples/camera/main/src/recorder/recorder.cpp)
每写一个 mp4 同时维护一份 `<name>.mp4.idx`，结构非常朴素：

```json
{
  "v": 1,
  "ts": 90000,
  "init": 1248,                       // ftyp + moov 总字节数（init segment 大小）
  "frags": [
    [0, 1248, 12345],                 // [tfdt(90kHz), byte_offset, size]
    [180000, 13593, 12010],
    [360000, 25603, 11888]
  ]
}
```

每片 mp4 的 tfdt 时基都从 0 开始（落盘时由 `RewriteTfdtInPlace_locked` 重写），
因此 `.idx` 里的 `tfdt` 等同于"自该片起播以来的播放时间 × ts"，前端做时间→fragment
映射只需一次 lower_bound。

**清理**：`RecordCleaner` 清理过期目录或超容量删 mp4 时，会同时 `unlink` 同名 `.mp4.idx`。

**回放端**：[web_player.html](file:///Users/bytedance/work/libmaix/examples/camera/web_player.html)
首次加载该片时一次 GET `.mp4.idx`（几 KB，瞬时返回）。之后每次 seek：

1. 若目标位置仍在已 append 的 SourceBuffer 范围内 → 直接 `video.currentTime = t` 不发请求
2. 否则按 `.idx` 找到目标 fragment 的 `[off, off+size)` → `Range: bytes=0-init-1` 拉 init seg
   （只第一次）+ `Range: bytes=off-(off+size-1)` 拉单个 fragment → `appendBuffer`
3. 整个过程不再重置 `<video>` 元素，CSS 已锁定容器尺寸，无视口闪烁

**`mediaSource.duration` 即时锁定**：原生 `<video>` 控件只有在 `duration` 是有限正数
时才会渲染总时长 + 启用进度条拖动。原始流程依赖"流式 append 到 EOF → endOfStream → MSE
自动算出 duration"，30 分钟一片在 1MB/s 上行带宽下要 30~60s 才结束，整段时间 slider 拖不动。
现在 init seg append 完后立刻按 `.idx` 估算总时长（`tfdt(last)/ts + 末段时长`）写到
`mediaSource.duration`，浏览器毫秒级拿到有限的总时长，slider 立即可拖。
为避免覆盖该值，idx 快路径不再调 `endOfStream()`，让 ms 全程保持 `open`。

**双向拖动**：精准跳转后只 append 了 `[fi, last]` 的字节，进度条向前拖到 `t < tfdt(fi)`
时目标不在 `SourceBuffer.buffered` 内，MSE 自身不会主动发请求（这是 `<video>` 的"哑"
seek 行为）。前端额外监听 `video.seeking` 事件，发现目标缺字节就用 `.idx` 定位前向缺口
`[fi'.off, fi.off-1]`，单次 Range 请求拉回来 append（`sb.mode='segments'` 会按 tfdt 自动归位），
再把 `currentTime` 重置一次让 video 命中——同样不重建 SourceBuffer / `<video>` 元素，
向前拖动也能毫秒级落点。

服务端配合：[httpServer](file:///Users/bytedance/work/libmaix/examples/camera/main/src/httpServer)
对所有 `filePath` 响应都解析 `Range:` 头返回 206，配合内核 `sendfile`/分块 `read+send`
保持 64KB 滚动缓冲。

> 旧版"整文件下载、客户端自己 demux"仍然兼容（无 `.idx` 时前端走老分支），
> 这也是为什么改造可以平滑铺设到既存录像目录上。

### 5.2 仅音频直播：`/ws/audio`（省带宽方案）

完整 fMP4 直播在 30FPS / VBR 5Mbps 下的实际下行约 **1~2 Mbps**。
当用户只想"听"现场（例如锁屏后台监听、车载弱网、流量计费场景）时，
完全没必要把视频字节也拉下来。为此服务端额外暴露一条独立的 WebSocket
路径 `/ws/audio`，**只推 AAC 音频**，下行带宽降到 **~64 kbps（AAC 码率本身）**。

#### 服务端链路

- 复用同一个 `C_WebSocketServer`（端口 8081 / 8444），不新增端口。
- 路径派发：[websocketServer.cpp](file:///Users/bytedance/work/libmaix/examples/camera/main/src/websocketServer/websocketServer.cpp)
  - `BroadcastBinary`（fmp4 视频路径）**跳过** `urlPath == "/ws/audio"` 的客户端
  - 新增 [`BroadcastAudioBinary`](file:///Users/bytedance/work/libmaix/examples/camera/main/src/websocketServer/websocketServer.cpp)：**只**向 `/ws/audio` 客户端发包
  - 握手成功时：`/ws/audio` 客户端**不下发** fmp4 init segment（拿到也没用）
- 帧来源：[terminal.cpp::OnOutputAac](file:///Users/bytedance/work/libmaix/examples/camera/main/src/terminal/terminal.cpp)
  在每帧 raw AAC 出来后，先用 `BuildAdtsFrame` 加 7 字节 ADTS 头，再调
  `m_pWsServer->BroadcastAudioBinary(...)`。
  - 这条路径**不依赖 muxer / 是否拿到首个 IDR**——只要 `aacEnc` 出帧就立即广播。
    所以浏览器端"仅音频模式"在视频通路尚未就绪时也能听到声音。
  - ADTS 头各字段从 `m_pAacEnc->SampleRate() / Channels()` 现取，与 `aacEnc` 内部
    的 dump 路径完全一致。

#### 浏览器端

[web_player.html](file:///Users/bytedance/work/libmaix/examples/camera/web_player.html) 顶栏新增 `🎧 仅音频` 按钮：

- 点击后 `stopLive()` 释放 fmp4 / MSE，再连 `ws(s)://host:8081|8444/ws/audio`
- 接到二进制帧后用 `AudioContext.decodeAudioData` 直接解 ADTS（Chrome / Edge / Firefox / Safari 原生支持）
- 用单调递增的 `audioPlayHead` 调度 `BufferSource.start(t)`，落后超 1.2s 自动追到 `now+50ms`
- 背压保护：同时排队 decode > 12 帧（约 256ms × 12）就丢一帧，避免 GC 抖动
- Safari/iOS：必须用户手势触发 `audioCtx.resume()`，因此固定走 `btnAudioOnly.onclick`

#### 与视频直播 / 对讲的关系

| 维度 | `/ws/live` (fmp4) | `/ws/audio` (ADTS AAC) |
|---|---|---|
| 下行带宽 | ~1-2 Mbps | ~64 kbps |
| 解码栈 | MSE + `<video>` | Web Audio API |
| 起播延迟 | 等首个 IDR | 立即（ALSA 出帧即广播） |
| 互斥 | 与 `/ws/audio` 互斥 | 进入仅音频时自动 `stopLive()` |
| 入流 | `Recorder + LiveHub.OnLiveFragment` | `BroadcastAudioBinary` 旁路 |

> **单一编码源**：板上始终只跑一份 H264 + 一份 AAC 编码，无论几个客户端订阅
> 视频/音频流——这一点和视频直播保持一致，64MB 板子才扛得住。


---

### 5.3 板上提示音播放：`POST /api/prompt`

需求场景：web 端点一下"放门口"按钮，开发板扬声器立即播一段固定提示音
（典型用途：外卖配送提示、欢迎语、告警语）。

#### 链路

```
[Web 端] 🔔 放门口 button
    │  POST /api/prompt   body: {"name":"door"}
    ▼
[HTTP 工作线程] terminal.cpp ──校验白名单(name 仅 [A-Za-z0-9_-])
    │                     ──拼路径 <exeDir>/prompt/<name>.wav
    │                     ──stat 文件存在 → 估算 duration_ms
    ▼
[C_TalkPlayer::PlayWavSync(path)] talkPlayer.cpp
    │  1. m_promptBusy CAS true     （重叠请求 → -3 → HTTP 409 busy）
    │  2. 一次性读 wav 入内存（≤8MB 上限）
    │  3. 解析 RIFF/WAVE：要求 PCM/48kHz/16bit/mono（其它格式 → -1 → HTTP 500）
    │  4. 若 m_pcm 未打开（没有 talk 客户端在线）→ EnsureAlsa_locked
    │  5. AlsaWrite 写 data 子块（受 m_pcmMtx 保护，与 talk 解码线程串行）
    │  6. snd_pcm_drain 等待播完
    │  7. 临时打开的 m_pcm 在播完后归还（前提：talk 仍未上线）
    ▼
[ALSA default] V831 内置扬声器
```

#### 资源管理（与对讲共存）

ALSA `default` 设备在 V831 上**独占**，整个工程对它只允许一条打开路径。
做法：

- **TalkPlayer 既管对讲、也管提示音**：第一个 `/ws/talk` 连入打开 ALSA，最后一个
  断开时按需释放；提示音播放复用同一 `m_pcm`。
- **m_pcmMtx 序列化两路写入**：talk 解码线程和 prompt 同步路径都在 period 粒度
  交替提交，等价于"先到先服务"的简易 mixer。
- **m_promptBusy CAS 守护并发**：同时只允许一段提示音在播，重叠请求直接 409
  避免噪声叠加 / underrun。
- **Shutdown 在 prompt 进行时跳过 close**：talk 客户端最后一个断开时若
  `m_promptBusy==true`，保留 m_pcm 让提示音播完，由 PlayWavSync 末尾收尾。

#### wav 文件部署

```
examples/camera/door.wav         ← 源文件（已纳入 git）
       │
       │  CMake copy_prompt_assets target  (file(GLOB) 整目录扫，新增 wav 不必改 CMake)
       ▼
dist/camera/prompt/door.wav      ← buildpush.sh 推到板子
       │
       ▼
/root/maix_dist/prompt/door.wav  ← C_TalkPlayer::PlayWavSync 加载
```

**格式约束（第一版严格校验）**：必须是 `PCM(format=1) / 48000Hz / 16-bit / 1ch`。
不匹配返回 -1 / HTTP 500。新增提示音时，先用 ffmpeg 转码：

```bash
ffmpeg -i in.wav -ar 48000 -ac 1 -sample_fmt s16 examples/camera/welcome.wav
```

后端代码无需任何改动，新文件随 buildpush.sh 一起部署即可，前端通过
`POST /api/prompt {name:"welcome"}` 触发（注意：前端按钮目前只硬编码了
"door"，添加新提示音时需要在 web_player.html 里加一个对应按钮）。

#### HTTP 响应码

| 状态 | body | 触发条件 |
|---|---|---|
| 200 | `{ok:true, name, duration_ms}` | 正常播放完成 |
| 400 | `{ok:false, err:"missing name"}` 或 `bad name` | 字段缺失 / 包含非白名单字符 |
| 404 | `{ok:false, err:"not found"}` | wav 文件不存在 |
| 409 | `{ok:false, err:"busy"}` | 另一段提示音正在播 |
| 500 | `{ok:false, err:"play failed"}` | wav 格式不符 / ALSA 错误 |

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
Mac (你)                ──rsync──>  192.168.1.10 (编译机)         ──scp──>  192.168.1.28 (M2dock)
~/work/libmaix          ─────────>   /root/work/libmaix          ────────>  /root/maix_dist
                                     python3 project.py build              start_app.sh
```

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
- 模型目录：`/root/models/awnn_yolo_person.{bin,param}`（缺失时 AI 模块不致命）
- 证书目录：`<exeDir>/cert/server.{crt,key}`（缺失时降级 HTTP/WS 明文）
- 前端文件：`<exeDir>/web/index.html`（CMake 把 `web_player.html` 复制过去）

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

## 11. 已知问题 / TODO

| 编号 | 问题 | 详情见"优化分析" |
|---|---|---|
| C1 | `main.cpp:192` `-Wmisleading-indentation` 告警 | `if x1 < 0; if y1 < 0;` 同行 |
| B1 | TLS 握手在 accept 线程 | 影响多 HTTPS 客户端接入延迟 |
| A1 | 每个 HTTP 连接 8MB 栈的线程 | 改线程池或缩栈到 256KB |
| A2 | `InputRgb888` / `rgb888ToNv21` / `m_pNv12Buff` 死代码 | 仅被 `#if 0` 旧路径引用 |
| A3 | `SimpleSHA1` / `SimpleBase64` 自实现 | OpenSSL 已链接，可换 `SHA1()` |
| C2 | `sleep_for(8s)` 等 IP / NTP | 改循环检测 + 可中断 |

---

## 12. 依赖与许可

- **libmaix**：摄像头 / 显示 / VO / NPU 抽象（同仓库根 `components/`）
- **FFmpeg / x264 / opus**：AAC 编码 + OPUS 解码（`dep/ffmpeg/`）
- **OpenSSL 1.1**：TLS（HTTPS / WSS）+ 自签名证书
- **OpenCV**：OSD 文字 / JPEG 编码（已由 libmaix 引入）
- **ALSA**：麦克风采集 + 对讲扬声器播放
- **awnn**：V831 NPU 推理后端（YOLOv2 person_int8）
