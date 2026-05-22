# Camera Rust

基于 Rust 的 M2dock 视频监控系统，是 [libmaix camera 例程](../camera) 的 Rust 语言实现。

## 功能特性

- 摄像头视频采集（640x480，NV21 格式）
- H264 硬件编码
- TCP 网络实时传输（端口 56050）
- 本地文件存储（带 I 帧索引，支持快进快退）
- 异步架构（基于 Tokio）
- 内存安全（Rust 所有权系统）

## 项目结构

```
camera-rust/
├── Cargo.toml              # Rust 项目配置
├── build.rs                # FFI 绑定生成脚本
├── wrapper.h               # C 头文件包装器
├── .cargo/
│   └── config.toml         # 交叉编译配置
└── src/
    ├── main.rs             # 程序入口
    ├── lib.rs              # 库导出
    ├── error.rs            # 错误类型定义
    ├── ffi/                # FFI 绑定模块
    │   └── mod.rs
    ├── camera.rs           # 摄像头模块
    ├── vo.rs               # 视频输出模块
    ├── encoder/            # 编码器模块
    │   ├── mod.rs
    │   └── h264.rs         # H264 硬件编码
    ├── server/             # 服务器模块
    │   ├── mod.rs
    │   ├── tcp.rs          # TCP 视频传输
    │   └── file.rs         # 文件管理
    └── terminal.rs         # 终端调度模块
```

## 与 C++ 版本对比

| 特性 | C++ 版本 | Rust 版本 |
|------|----------|-----------|
| 内存安全 | 手动管理 | 编译期保证 |
| 并发安全 | 需小心处理 | 所有权系统保护 |
| 异步处理 | 线程 + sleep | Tokio 异步运行时 |
| 错误处理 | 返回值检查 | Result + ? 运算符 |
| 代码量 | 较多 | 较少（现代语法）|
| 性能 | 优秀 | 零成本抽象，同等优秀 |

## 编译要求

### 1. 安装 Rust 工具链

```bash
# 安装 rustup
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh

# 添加 ARM 目标
rustup target add armv7-unknown-linux-gnueabihf
```

### 2. 安装交叉编译工具链

```bash
# Ubuntu/Debian
sudo apt-get install gcc-arm-linux-gnueabihf

# 或使用 prebuilt 工具链
# 下载 Allwinner V831 工具链并配置环境变量
```

### 3. 配置环境变量

```bash
export LIBMAIX_SDK_PATH=/root/work/libmaix
export LIBMAIX_TOOLCHAIN_PATH=/path/to/toolchain/bin
```

## 编译步骤

```bash
cd /root/work/libmaix/examples/camera-rust

# 生成 FFI 绑定并编译
cargo build --release

# 生成的二进制文件
# target/armv7-unknown-linux-gnueabihf/release/camera-rust
```

## 运行

### 在开发板上运行

```bash
# 复制到开发板
scp target/armv7-unknown-linux-gnueabihf/release/camera-rust root@m2dock-ip:/root/

# 在开发板上运行
./camera-rust
```

### 查看视频流

使用 VLC 或 ffplay 连接 TCP 端口：

```bash
# 使用 ffplay
ffplay tcp://m2dock-ip:56050

# 或使用 VLC
vlc tcp://m2dock-ip:56050
```

## 协议说明

### TCP 视频流协议

```
┌────────────────┬────────────┬──────────────┐
│ 4字节长度      │ 1字节标志  │ N字节数据    │
│ (网络字节序)   │            │              │
└────────────────┴────────────┴──────────────┘
```

- **标志位 `0x80`**：视频数据（H264）
- **标志位 `0x00`**：音频数据（Opus）

### 文件存储格式

```
┌─────────────────────────┬───────────┬───────────┬─────┐
│ I帧索引头               │ 数据块1   │ 数据块2   │ ... │
│ (8字节 × 100)           │           │           │     │
└─────────────────────────┴───────────┴───────────┴─────┘
```

## 架构说明

### 数据流向

```
摄像头采集 (NV21)
    │
    ▼
VideoOutput (零拷贝缓冲区)
    │
    ▼
Terminal::process_frame()
    │
    ├──→ H264Encoder (硬件编码)
    │           │
    │           ▼
    │    TerminalOutput::on_output()
    │           │
    │     ┌─────┴─────┐
    │     ▼           ▼
    │ TcpServer   FileManager
    │ (网络传输)   (本地存储)
    │     │           │
    │     ▼           ▼
    │  客户端      视频文件
    │
    ▼
屏幕显示 (240x240)
```

### 异步架构

```
┌─────────────────────────────────────────┐
│              Tokio Runtime              │
├─────────────────────────────────────────┤
│  ┌─────────┐  ┌─────────┐  ┌─────────┐ │
│  │ Main    │  │ TCP     │  │ File    │ │
│  │ Task    │  │ Server  │  │ Writer  │ │
│  │         │  │ Task    │  │ Task    │ │
│  │ •Camera │  │         │  │         │ │
│  │ •Encode │  │ •Accept │  │ •Write  │ │
│  │ •Display│  │ •Send   │  │ •Rotate │ │
│  └─────────┘  └─────────┘  └─────────┘ │
└─────────────────────────────────────────┘
```

## 关键设计

### 1. 零拷贝采集

```rust
// 摄像头直接采集到 VO 缓冲区
let y_plane = frame.y_plane_mut(width, height);
camera.capture(y_plane)?;
```

### 2. 安全的 FFI 封装

```rust
pub struct Camera {
    inner: NonNull<ffi::libmaix_cam>,
}

impl Drop for Camera {
    fn drop(&mut self) {
        unsafe {
            let mut cam = self.inner.as_ptr();
            ffi::libmaix_cam_destroy(&mut cam);
        }
    }
}
```

### 3. 异步 TCP 服务器

```rust
pub async fn run(&self) -> anyhow::Result<()> {
    let listener = TcpListener::bind(&addr).await?;
    
    loop {
        let (stream, addr) = listener.accept().await?;
        tokio::spawn(async move {
            handle_client(stream, addr).await
        });
    }
}
```

## 待完善功能

- [ ] Opus 音频编码
- [ ] OSD 叠加（IP 地址、时间）
- [ ] 文件回放服务器
- [ ] 配置热更新
- [ ] WebRTC 支持

## 调试

```bash
# 启用详细日志
RUST_LOG=debug ./camera-rust

# 使用 GDB 调试
gdb-multiarch ./camera-rust
```

## 参考

- [libmaix 文档](https://github.com/sipeed/libmaix)
- [Rust Embedded Book](https://doc.rust-lang.org/stable/embedded-book/)
- [Tokio 文档](https://tokio.rs/)

## 许可证

MIT OR Apache-2.0
