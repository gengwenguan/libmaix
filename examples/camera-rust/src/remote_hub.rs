use crate::config::{AppConfig, RuntimeConfig};
use crate::native::NativeEngine;
use crate::netinfo;
use crate::snapshot::SnapshotService;
use anyhow::{bail, Context, Result};
use base64::engine::general_purpose::STANDARD;
use base64::Engine;
use serde::Serialize;
use serde_json::json;
use sha1::{Digest, Sha1};
use std::collections::VecDeque;
use std::io::{Read, Write};
use std::net::{Ipv6Addr, TcpStream, ToSocketAddrs};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Condvar, Mutex};
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

const HTTP_TIMEOUT: Duration = Duration::from_secs(5);
const FRAME_QUEUE_ITEMS: usize = 512;
const FRAME_QUEUE_BYTES: usize = 8 * 1024 * 1024;

const FRAME_H264: u8 = 1;
const FRAME_AAC: u8 = 2;
const FRAME_PHOTO: u8 = 0x81;

#[derive(Clone, Debug, Serialize)]
pub struct RemoteHubStatus {
    pub enabled: bool,
    pub connected: bool,
    pub configured_url: String,
    pub resolved_url: String,
    pub uploaded_frames: u64,
    pub dropped_frames: u64,
    pub synced_photos: u64,
    pub last_error: String,
}

impl Default for RemoteHubStatus {
    fn default() -> Self {
        Self {
            enabled: false,
            connected: false,
            configured_url: String::new(),
            resolved_url: String::new(),
            uploaded_frames: 0,
            dropped_frames: 0,
            synced_photos: 0,
            last_error: String::new(),
        }
    }
}

struct MediaQueueState {
    frames: VecDeque<FramePacket>,
    frame_bytes: usize,
    dropped_frames: u64,
}

struct FramePacket {
    kind: u8,
    flags: u16,
    pts_us: i64,
    capture_epoch_us: i64,
    data: Vec<u8>,
}

pub struct HubMediaInput {
    config: Arc<RuntimeConfig>,
    state: Mutex<MediaQueueState>,
    ready: Condvar,
}

impl HubMediaInput {
    pub fn new(config: Arc<RuntimeConfig>) -> Arc<Self> {
        Arc::new(Self {
            config,
            state: Mutex::new(MediaQueueState {
                frames: VecDeque::with_capacity(FRAME_QUEUE_ITEMS),
                frame_bytes: 0,
                dropped_frames: 0,
            }),
            ready: Condvar::new(),
        })
    }

    pub fn push_h264(&self, data: &[u8], pts_us: i64, is_key: bool) {
        self.push_encoded(FRAME_H264, u16::from(is_key), pts_us, data);
    }

    pub fn push_aac(&self, data: &[u8], pts_us: i64) {
        self.push_encoded(FRAME_AAC, 0, pts_us, data);
    }

    fn push_encoded(&self, kind: u8, flags: u16, pts_us: i64, data: &[u8]) {
        let current = self.config.current();
        if !remote_media_requested(&current) || data.is_empty() {
            return;
        }
        let mut state = self.state.lock().unwrap_or_else(|error| error.into_inner());
        push_frame_locked(
            &mut state,
            FramePacket {
                kind,
                flags,
                pts_us,
                capture_epoch_us: epoch_us(),
                data: data.to_vec(),
            },
        );
        self.ready.notify_one();
    }

    fn pop_frame(&self, timeout: Duration) -> Option<FramePacket> {
        let state = self.state.lock().unwrap_or_else(|error| error.into_inner());
        let mut state = self
            .ready
            .wait_timeout_while(state, timeout, |state| state.frames.is_empty())
            .unwrap_or_else(|error| error.into_inner())
            .0;
        let frame = state.frames.pop_front();
        if let Some(frame) = &frame {
            state.frame_bytes = state.frame_bytes.saturating_sub(frame.data.len());
        }
        frame
    }

    fn dropped_frames(&self) -> u64 {
        self.state
            .lock()
            .unwrap_or_else(|error| error.into_inner())
            .dropped_frames
    }
}

fn push_frame_locked(state: &mut MediaQueueState, packet: FramePacket) {
    while !state.frames.is_empty()
        && (state.frames.len() >= FRAME_QUEUE_ITEMS
            || state.frame_bytes.saturating_add(packet.data.len()) > FRAME_QUEUE_BYTES)
    {
        if let Some(dropped) = state.frames.pop_front() {
            state.frame_bytes = state.frame_bytes.saturating_sub(dropped.data.len());
            state.dropped_frames = state.dropped_frames.saturating_add(1);
        }
    }
    state.frame_bytes = state.frame_bytes.saturating_add(packet.data.len());
    state.frames.push_back(packet);
}

pub struct RemoteHub {
    engine: Arc<NativeEngine>,
    status: Arc<Mutex<RemoteHubStatus>>,
    stop: Arc<AtomicBool>,
    threads: Vec<JoinHandle<()>>,
}

impl RemoteHub {
    pub fn start(
        config: Arc<RuntimeConfig>,
        media: Arc<HubMediaInput>,
        engine: Arc<NativeEngine>,
        snapshot: Arc<SnapshotService>,
    ) -> Result<Self> {
        let status = Arc::new(Mutex::new(RemoteHubStatus::default()));
        let stop = Arc::new(AtomicBool::new(false));
        let frame_thread = {
            let config = config.clone();
            let media = media.clone();
            let status = status.clone();
            let stop = stop.clone();
            let engine = engine.clone();
            let frame_snapshot = snapshot.clone();
            thread::Builder::new()
                .name("camera-hub-frame-link".to_owned())
                .stack_size(384 * 1024)
                .spawn(move || {
                    frame_link_loop(config, media, engine, frame_snapshot, status, stop)
                })?
        };
        Ok(Self {
            engine,
            status,
            stop,
            threads: vec![frame_thread],
        })
    }

    pub fn status(&self) -> RemoteHubStatus {
        self.status
            .lock()
            .unwrap_or_else(|error| error.into_inner())
            .clone()
    }

    pub fn apply_config(&self, config: &AppConfig) -> Result<()> {
        self.engine.set_config(config)
    }
}

impl Drop for RemoteHub {
    fn drop(&mut self) {
        self.stop.store(true, Ordering::Release);
        for thread in self.threads.drain(..) {
            let _ = thread.join();
        }
    }
}

fn frame_link_loop(
    config: Arc<RuntimeConfig>,
    media: Arc<HubMediaInput>,
    engine: Arc<NativeEngine>,
    snapshot: Arc<SnapshotService>,
    status: Arc<Mutex<RemoteHubStatus>>,
    stop: Arc<AtomicBool>,
) {
    while !stop.load(Ordering::Acquire) {
        let current = config.current();
        if !remote_media_requested(&current) {
            engine.set_remote_video_active(false);
            set_frame_link_status(&status, &media, &current, false, None);
            sleep_interruptible(&stop, Duration::from_secs(1));
            continue;
        }
        let result = resolved_endpoint(&current).and_then(|endpoint| {
            let mut websocket = FrameWebSocket::connect(&endpoint, &current.camera_hub_device_id)?;
            engine.set_remote_video_active(true);
            let mut sequence = 1u32;
            let mut last_hello = Instant::now() - Duration::from_secs(30);
            set_frame_link_status(&status, &media, &current, true, None);
            loop {
                if stop.load(Ordering::Acquire) {
                    return Ok(());
                }
                let latest = config.current();
                if !remote_media_requested(&latest)
                    || latest.camera_hub_url != current.camera_hub_url
                    || latest.camera_hub_follow_board_prefix
                        != current.camera_hub_follow_board_prefix
                    || latest.camera_hub_device_id != current.camera_hub_device_id
                {
                    return Ok(());
                }
                if last_hello.elapsed() >= Duration::from_secs(10) {
                    let hello = json!({
                        "type": "hello",
                        "firmware": env!("CARGO_PKG_VERSION"),
                        "ipv6": netinfo::global_ipv6(&latest.mqtt_iface),
                        "last_photo": snapshot.latest_camera_hub_source_name(),
                    });
                    websocket.send_text(&hello.to_string())?;
                    last_hello = Instant::now();
                }
                if let Some(packet) = media.pop_frame(Duration::from_millis(5)) {
                    websocket.send_binary(&encode_link_packet(sequence, &packet))?;
                    sequence = sequence.wrapping_add(1);
                    let mut value = status.lock().unwrap_or_else(|error| error.into_inner());
                    value.uploaded_frames = value.uploaded_frames.saturating_add(1);
                }
                while let Some((opcode, payload)) = websocket.read_message()? {
                    match opcode {
                        0x1 => {
                            if let Some(response) = clock_sync_response(&payload) {
                                websocket.send_text(&response)?;
                            }
                        }
                        0x2 => handle_link_binary(&payload, &snapshot, &status),
                        0x9 => {
                            let _ = websocket.send_control(0xA, &payload);
                        }
                        0x8 => bail!("camera-hub closed frame link"),
                        _ => {}
                    }
                }
                set_frame_link_status(&status, &media, &latest, true, None);
            }
        });
        engine.set_remote_video_active(false);
        set_frame_link_status(
            &status,
            &media,
            &current,
            false,
            result.err().map(|error| format!("frame link: {error:#}")),
        );
        sleep_interruptible(&stop, Duration::from_secs(1));
    }
}

fn set_frame_link_status(
    status: &Mutex<RemoteHubStatus>,
    media: &HubMediaInput,
    config: &AppConfig,
    connected: bool,
    error: Option<String>,
) {
    let mut value = status.lock().unwrap_or_else(|error| error.into_inner());
    value.enabled = remote_media_requested(config);
    value.configured_url.clone_from(&config.camera_hub_url);
    value.resolved_url = resolved_endpoint(config)
        .map(|endpoint| endpoint.url())
        .unwrap_or_default();
    value.connected = connected;
    if connected {
        value.last_error.clear();
    }
    value.dropped_frames = media.dropped_frames();
    if let Some(error) = error {
        value.last_error = error;
    }
}

fn clock_sync_response(data: &[u8]) -> Option<String> {
    let source_receive_epoch_us = epoch_us();
    let value = serde_json::from_slice::<serde_json::Value>(data).ok()?;
    if value.get("type")?.as_str()? != "clock_sync_request" {
        return None;
    }
    let server_send_epoch_us = value.get("server_send_epoch_us")?.as_i64()?;
    Some(
        json!({
            "type": "clock_sync_response",
            "server_send_epoch_us": server_send_epoch_us,
            "source_receive_epoch_us": source_receive_epoch_us,
            "source_send_epoch_us": epoch_us(),
        })
        .to_string(),
    )
}

fn handle_link_binary(data: &[u8], snapshot: &SnapshotService, status: &Mutex<RemoteHubStatus>) {
    let Ok((kind, _, _, _, payload)) = decode_link_packet(data) else {
        return;
    };
    if kind != FRAME_PHOTO || payload.len() < 2 {
        return;
    }
    let name_len = u16::from_be_bytes([payload[0], payload[1]]) as usize;
    if payload.len() < 2 + name_len {
        return;
    }
    let Ok(name) = std::str::from_utf8(&payload[2..2 + name_len]) else {
        return;
    };
    if snapshot
        .import_camera_hub_jpeg(name, &payload[2 + name_len..])
        .is_ok()
    {
        let mut value = status.lock().unwrap_or_else(|error| error.into_inner());
        value.synced_photos = value.synced_photos.saturating_add(1);
    }
}

fn encode_link_packet(sequence: u32, packet: &FramePacket) -> Vec<u8> {
    let mut output = Vec::with_capacity(32 + packet.data.len());
    output.extend_from_slice(b"CHP1");
    output.push(packet.kind);
    output.push(2);
    output.extend_from_slice(&packet.flags.to_be_bytes());
    output.extend_from_slice(&sequence.to_be_bytes());
    output.extend_from_slice(&packet.pts_us.to_be_bytes());
    output.extend_from_slice(&(packet.data.len() as u32).to_be_bytes());
    output.extend_from_slice(&packet.capture_epoch_us.to_be_bytes());
    output.extend_from_slice(&packet.data);
    output
}

fn epoch_us() -> i64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|value| i64::try_from(value.as_micros()).unwrap_or(i64::MAX))
        .unwrap_or_default()
}

fn decode_link_packet(data: &[u8]) -> Result<(u8, u16, u32, i64, &[u8])> {
    if data.len() < 24 || &data[..4] != b"CHP1" || data[5] != 1 {
        bail!("invalid frame link packet");
    }
    let flags = u16::from_be_bytes([data[6], data[7]]);
    let sequence = u32::from_be_bytes(data[8..12].try_into()?);
    let pts_us = i64::from_be_bytes(data[12..20].try_into()?);
    let length = u32::from_be_bytes(data[20..24].try_into()?) as usize;
    if data.len() != 24 + length {
        bail!("invalid frame link payload length");
    }
    Ok((data[4], flags, sequence, pts_us, &data[24..]))
}

struct FrameWebSocket {
    stream: TcpStream,
    reader: TcpStream,
    received: Vec<u8>,
    mask_sequence: u32,
}

impl FrameWebSocket {
    fn connect(endpoint: &Endpoint, device_id: &str) -> Result<Self> {
        let mut stream = endpoint.connect()?;
        stream.set_read_timeout(Some(HTTP_TIMEOUT))?;
        stream.set_write_timeout(Some(HTTP_TIMEOUT))?;
        let nonce = Instant::now().elapsed().as_nanos() as u64
            ^ u64::from(std::process::id())
            ^ (device_id.len() as u64).rotate_left(17);
        let key = STANDARD.encode(nonce.to_be_bytes());
        let host = if endpoint.host.contains(':') {
            format!("[{}]:{}", endpoint.host, endpoint.port)
        } else {
            format!("{}:{}", endpoint.host, endpoint.port)
        };
        let path = format!(
            "{}/api/v1/devices/{}/link",
            endpoint.base_path.trim_end_matches('/'),
            device_id
        );
        let request = format!(
            "GET {path} HTTP/1.1\r\nHost: {host}\r\nUpgrade: websocket\r\n\
             Connection: Upgrade\r\nSec-WebSocket-Version: 13\r\n\
             Sec-WebSocket-Key: {key}\r\n\r\n"
        );
        stream.write_all(request.as_bytes())?;
        let mut response = Vec::new();
        loop {
            if response.windows(4).any(|window| window == b"\r\n\r\n") {
                break;
            }
            let mut buffer = [0u8; 1024];
            let size = stream.read(&mut buffer)?;
            if size == 0 || response.len() + size > 16 * 1024 {
                bail!("invalid frame link handshake");
            }
            response.extend_from_slice(&buffer[..size]);
        }
        let header = std::str::from_utf8(&response)?;
        if !header.starts_with("HTTP/1.1 101 ") {
            bail!("frame link upgrade failed");
        }
        let expected = websocket_accept(&key);
        if !header.lines().any(|line| {
            line.split_once(':').is_some_and(|(name, value)| {
                name.eq_ignore_ascii_case("Sec-WebSocket-Accept") && value.trim() == expected
            })
        }) {
            bail!("frame link accept key mismatch");
        }
        let reader = stream.try_clone()?;
        reader.set_nonblocking(true)?;
        stream.set_read_timeout(None)?;
        Ok(Self {
            stream,
            reader,
            received: Vec::with_capacity(64 * 1024),
            mask_sequence: 1,
        })
    }

    fn send_text(&mut self, text: &str) -> Result<()> {
        self.send_control(0x1, text.as_bytes())
    }

    fn send_binary(&mut self, data: &[u8]) -> Result<()> {
        self.send_control(0x2, data)
    }

    fn send_control(&mut self, opcode: u8, data: &[u8]) -> Result<()> {
        let mask = self.mask_sequence.to_be_bytes();
        self.mask_sequence = self.mask_sequence.wrapping_add(1);
        let mut frame = Vec::with_capacity(data.len() + 14);
        frame.push(0x80 | opcode);
        if data.len() < 126 {
            frame.push(0x80 | data.len() as u8);
        } else if data.len() <= u16::MAX as usize {
            frame.push(0x80 | 126);
            frame.extend_from_slice(&(data.len() as u16).to_be_bytes());
        } else {
            frame.push(0x80 | 127);
            frame.extend_from_slice(&(data.len() as u64).to_be_bytes());
        }
        frame.extend_from_slice(&mask);
        frame.extend(
            data.iter()
                .enumerate()
                .map(|(index, byte)| byte ^ mask[index % 4]),
        );
        self.stream.write_all(&frame)?;
        Ok(())
    }

    fn read_message(&mut self) -> Result<Option<(u8, Vec<u8>)>> {
        let mut buffer = [0u8; 8192];
        loop {
            match self.reader.read(&mut buffer) {
                Ok(0) => bail!("frame link closed"),
                Ok(size) => self.received.extend_from_slice(&buffer[..size]),
                Err(error)
                    if matches!(
                        error.kind(),
                        std::io::ErrorKind::WouldBlock | std::io::ErrorKind::TimedOut
                    ) =>
                {
                    break;
                }
                Err(error) => return Err(error.into()),
            }
        }
        let Some((opcode, payload, consumed)) = parse_server_frame(&self.received)? else {
            return Ok(None);
        };
        self.received.drain(..consumed);
        Ok(Some((opcode, payload)))
    }
}

fn parse_server_frame(data: &[u8]) -> Result<Option<(u8, Vec<u8>, usize)>> {
    if data.len() < 2 {
        return Ok(None);
    }
    let opcode = data[0] & 0x0f;
    let masked = data[1] & 0x80 != 0;
    let mut offset = 2usize;
    let mut length = usize::from(data[1] & 0x7f);
    if length == 126 {
        if data.len() < 4 {
            return Ok(None);
        }
        length = u16::from_be_bytes([data[2], data[3]]) as usize;
        offset = 4;
    } else if length == 127 {
        if data.len() < 10 {
            return Ok(None);
        }
        length = u64::from_be_bytes(data[2..10].try_into()?) as usize;
        offset = 10;
    }
    if length > 2 * 1024 * 1024 {
        bail!("frame link message too large");
    }
    let mask = if masked {
        if data.len() < offset + 4 {
            return Ok(None);
        }
        let mask = [
            data[offset],
            data[offset + 1],
            data[offset + 2],
            data[offset + 3],
        ];
        offset += 4;
        Some(mask)
    } else {
        None
    };
    if data.len() < offset + length {
        return Ok(None);
    }
    let mut payload = data[offset..offset + length].to_vec();
    if let Some(mask) = mask {
        for (index, byte) in payload.iter_mut().enumerate() {
            *byte ^= mask[index % 4];
        }
    }
    Ok(Some((opcode, payload, offset + length)))
}

fn websocket_accept(key: &str) -> String {
    let mut digest = Sha1::new();
    digest.update(key.as_bytes());
    digest.update(b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
    STANDARD.encode(digest.finalize())
}

fn remote_media_requested(config: &AppConfig) -> bool {
    !config.camera_hub_url.trim().is_empty()
}

fn sleep_interruptible(stop: &AtomicBool, duration: Duration) {
    let mut remaining = duration;
    while !stop.load(Ordering::Acquire) && !remaining.is_zero() {
        let slice = remaining.min(Duration::from_millis(250));
        thread::sleep(slice);
        remaining = remaining.saturating_sub(slice);
    }
}

struct Endpoint {
    host: String,
    port: u16,
    base_path: String,
}

impl Endpoint {
    fn url(&self) -> String {
        let host = if self.host.contains(':') {
            format!("[{}]", self.host)
        } else {
            self.host.clone()
        };
        format!("http://{host}:{}{}", self.port, self.base_path)
    }

    fn connect(&self) -> Result<TcpStream> {
        let stream = (self.host.as_str(), self.port)
            .to_socket_addrs()?
            .find_map(|address| TcpStream::connect_timeout(&address, HTTP_TIMEOUT).ok())
            .context("connect camera-hub")?;
        stream.set_nodelay(true)?;
        Ok(stream)
    }
}

fn resolved_endpoint(config: &crate::config::AppConfig) -> Result<Endpoint> {
    let mut endpoint = parse_http_url(&config.camera_hub_url)?;
    if config.camera_hub_follow_board_prefix {
        let board = netinfo::global_ipv6(&config.mqtt_iface);
        if let (Ok(board), Ok(target)) =
            (board.parse::<Ipv6Addr>(), endpoint.host.parse::<Ipv6Addr>())
        {
            endpoint.host = follow_prefix(board, target).to_string();
        }
    }
    Ok(endpoint)
}

fn follow_prefix(board: Ipv6Addr, target: Ipv6Addr) -> Ipv6Addr {
    let board = board.segments();
    let target = target.segments();
    Ipv6Addr::new(
        board[0], board[1], board[2], board[3], target[4], target[5], target[6], target[7],
    )
}

fn parse_http_url(url: &str) -> Result<Endpoint> {
    let remainder = url
        .trim()
        .strip_prefix("http://")
        .context("camera-hub URL must start with http://")?;
    let (authority, path) = remainder
        .split_once('/')
        .map_or((remainder, String::new()), |(authority, path)| {
            (authority, format!("/{path}"))
        });
    let (host, port) = if let Some(ipv6) = authority.strip_prefix('[') {
        let end = ipv6.find(']').context("invalid bracketed IPv6 URL")?;
        let host = ipv6[..end].to_owned();
        let suffix = &ipv6[end + 1..];
        let port = suffix
            .strip_prefix(':')
            .unwrap_or("80")
            .parse::<u16>()
            .context("invalid camera-hub port")?;
        (host, port)
    } else if let Some((host, port)) = authority.rsplit_once(':') {
        if host.contains(':') {
            bail!("camera-hub IPv6 address must use brackets");
        }
        (host.to_owned(), port.parse::<u16>()?)
    } else {
        (authority.to_owned(), 80)
    };
    if host.is_empty() {
        bail!("camera-hub URL is missing host");
    }
    Ok(Endpoint {
        host,
        port,
        base_path: path,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn follows_board_slash_64_prefix() {
        let board = "2409:8a1e:7a52:abcd:a22c:36ff:febd:4feb".parse().unwrap();
        let target = "2409:8a1e:7a52:c9b0:528f:4cff:feef:dd90".parse().unwrap();
        assert_eq!(
            follow_prefix(board, target).to_string(),
            "2409:8a1e:7a52:abcd:528f:4cff:feef:dd90"
        );
    }

    #[test]
    fn parses_bracketed_ipv6_url() {
        let endpoint =
            parse_http_url("http://[2409:8a1e:7a52:c9b0:528f:4cff:feef:dd90]/base").unwrap();
        assert_eq!(endpoint.port, 80);
        assert_eq!(endpoint.base_path, "/base");
    }

    #[test]
    fn configured_url_controls_upload() {
        let mut config = AppConfig::default();
        assert!(remote_media_requested(&config));
        config.camera_hub_url.clear();
        assert!(!remote_media_requested(&config));
    }

    #[test]
    fn encodes_media_with_chp1_v2_capture_clock() {
        let packet = FramePacket {
            kind: FRAME_H264,
            flags: 1,
            pts_us: 123_456,
            capture_epoch_us: 1_765_000_000_123_456,
            data: vec![1, 2, 3],
        };
        let encoded = encode_link_packet(7, &packet);
        assert_eq!(&encoded[..4], b"CHP1");
        assert_eq!(encoded[5], 2);
        assert_eq!(u32::from_be_bytes(encoded[8..12].try_into().unwrap()), 7);
        assert_eq!(
            i64::from_be_bytes(encoded[24..32].try_into().unwrap()),
            packet.capture_epoch_us
        );
        assert_eq!(&encoded[32..], packet.data);
    }

    #[test]
    fn answers_clock_sync_request_with_source_timestamps() {
        let response =
            clock_sync_response(br#"{"type":"clock_sync_request","server_send_epoch_us":123456}"#)
                .unwrap();
        let value: serde_json::Value = serde_json::from_str(&response).unwrap();
        assert_eq!(value["type"], "clock_sync_response");
        assert_eq!(value["server_send_epoch_us"], 123456);
        assert!(value["source_receive_epoch_us"].as_i64().unwrap() > 0);
        assert!(
            value["source_send_epoch_us"].as_i64().unwrap()
                >= value["source_receive_epoch_us"].as_i64().unwrap()
        );
    }
}
