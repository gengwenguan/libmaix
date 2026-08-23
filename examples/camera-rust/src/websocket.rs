use crate::app::AppState;
use crate::hub::Subscription;
use crate::io::Connection;
use crate::native::TlsContext;
use anyhow::{Context, Result};
use base64::engine::general_purpose::STANDARD;
use base64::Engine;
use sha1::{Digest, Sha1};
use std::net::TcpListener;
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};
use std::thread::{self, JoinHandle};
use std::time::Duration;

const MAX_HANDSHAKE: usize = 16 * 1024;
const MAX_CLIENT_PAYLOAD: usize = 64 * 1024;
const WRITE_TIMEOUT: Duration = Duration::from_secs(5);

pub struct WebSocketServers {
    shutdown: Arc<AtomicBool>,
    accept_threads: Vec<JoinHandle<()>>,
    clients: Arc<Mutex<Vec<JoinHandle<()>>>>,
}

impl WebSocketServers {
    pub fn start(state: Arc<AppState>, tls: Option<Arc<TlsContext>>) -> Result<Self> {
        let shutdown = Arc::new(AtomicBool::new(false));
        let clients = Arc::new(Mutex::new(Vec::new()));
        let active = Arc::new(AtomicUsize::new(0));
        let mut accept_threads = Vec::new();

        for (port, listener_tls) in [
            (8081, None),
            (8082, None),
            (8444, tls.clone()),
            (8445, tls.clone()),
        ] {
            if port >= 8444 && listener_tls.is_none() {
                continue;
            }
            let listener =
                bind_dual_stack(port).with_context(|| format!("bind WebSocket port {port}"))?;
            let state = state.clone();
            let shutdown_flag = shutdown.clone();
            let client_threads = clients.clone();
            let active_clients = active.clone();
            let handle = thread::Builder::new()
                .name(format!("ws-accept-{port}"))
                .stack_size(192 * 1024)
                .spawn(move || {
                    accept_loop(
                        listener,
                        listener_tls,
                        state,
                        shutdown_flag,
                        client_threads,
                        active_clients,
                    )
                })?;
            accept_threads.push(handle);
        }

        Ok(Self {
            shutdown,
            accept_threads,
            clients,
        })
    }
}

impl Drop for WebSocketServers {
    fn drop(&mut self) {
        self.shutdown.store(true, Ordering::Release);
        for thread in self.accept_threads.drain(..) {
            let _ = thread.join();
        }
        let clients = std::mem::take(
            &mut *self
                .clients
                .lock()
                .unwrap_or_else(|error| error.into_inner()),
        );
        for client in clients {
            let _ = client.join();
        }
    }
}

fn bind_dual_stack(port: u16) -> Result<TcpListener> {
    let listener =
        TcpListener::bind(("::", port)).or_else(|_| TcpListener::bind(("0.0.0.0", port)))?;
    listener.set_nonblocking(true)?;
    Ok(listener)
}

fn accept_loop(
    listener: TcpListener,
    tls: Option<Arc<TlsContext>>,
    state: Arc<AppState>,
    shutdown: Arc<AtomicBool>,
    clients: Arc<Mutex<Vec<JoinHandle<()>>>>,
    active: Arc<AtomicUsize>,
) {
    while !shutdown.load(Ordering::Acquire) {
        match listener.accept() {
            Ok((stream, _)) => {
                if active.fetch_add(1, Ordering::AcqRel) >= 8 {
                    active.fetch_sub(1, Ordering::AcqRel);
                    continue;
                }
                let state = state.clone();
                let shutdown = shutdown.clone();
                let tls = tls.clone();
                let active_for_thread = active.clone();
                match thread::Builder::new()
                    .name("ws-client".to_owned())
                    .stack_size(256 * 1024)
                    .spawn(move || {
                        let result = Connection::new(stream, tls)
                            .and_then(|connection| handle_client(connection, state, shutdown));
                        if let Err(error) = result {
                            eprintln!("WebSocket client failed: {error:#}");
                        }
                        active_for_thread.fetch_sub(1, Ordering::AcqRel);
                    }) {
                    Ok(handle) => clients
                        .lock()
                        .unwrap_or_else(|error| error.into_inner())
                        .push(handle),
                    Err(error) => {
                        active.fetch_sub(1, Ordering::AcqRel);
                        eprintln!("spawn WebSocket client failed: {error}");
                    }
                }
            }
            Err(error) if error.kind() == std::io::ErrorKind::WouldBlock => {
                reap_clients(&clients);
                thread::sleep(Duration::from_millis(20));
            }
            Err(error) => {
                eprintln!("WebSocket accept failed: {error}");
                thread::sleep(Duration::from_millis(100));
            }
        }
    }
}

fn reap_clients(clients: &Mutex<Vec<JoinHandle<()>>>) {
    let mut clients = clients.lock().unwrap_or_else(|error| error.into_inner());
    let mut pending = Vec::with_capacity(clients.len());
    for client in clients.drain(..) {
        if client.is_finished() {
            let _ = client.join();
        } else {
            pending.push(client);
        }
    }
    *clients = pending;
}

enum Source {
    Live(Subscription),
    Audio(Subscription),
    Logs(Subscription),
    Talk,
}

fn handle_client(
    mut connection: Connection,
    state: Arc<AppState>,
    shutdown: Arc<AtomicBool>,
) -> Result<()> {
    let request = connection.read_until(b"\r\n\r\n", MAX_HANDSHAKE, Duration::from_secs(5))?;
    let (path, key) = parse_handshake(&request)?;
    let accept = websocket_accept(&key);
    let response = format!(
        "HTTP/1.1 101 Switching Protocols\r\n\
         Upgrade: websocket\r\n\
         Connection: Upgrade\r\n\
         Sec-WebSocket-Accept: {accept}\r\n\r\n"
    );
    connection.write_all_timeout(response.as_bytes(), WRITE_TIMEOUT)?;

    let source = match path.as_str() {
        "/" | "/ws/live" | "/ws/live/" => {
            state.engine.force_iframe();
            Source::Live(state.live.subscribe())
        }
        "/ws/audio" | "/ws/audio/" => Source::Audio(state.audio.subscribe()),
        "/ws/log" | "/ws/log/" => Source::Logs(state.logs.subscribe()),
        "/ws/talk" | "/ws/talk/" => {
            state.talk_connected()?;
            Source::Talk
        }
        _ => anyhow::bail!("unsupported WebSocket path {path}"),
    };
    let is_talk = matches!(source, Source::Talk);
    let result = websocket_loop(&mut connection, source, &state, &shutdown);
    if is_talk {
        state.talk_disconnected();
    }
    connection.shutdown();
    result
}

fn parse_handshake(request: &[u8]) -> Result<(String, String)> {
    let request = std::str::from_utf8(request).context("WebSocket request is not UTF-8")?;
    let mut lines = request.split("\r\n");
    let first = lines.next().context("missing request line")?;
    let mut parts = first.split_whitespace();
    if parts.next() != Some("GET") {
        anyhow::bail!("WebSocket handshake is not GET");
    }
    let path = parts.next().context("missing WebSocket path")?.to_owned();
    let mut key = None;
    let mut upgrade = false;
    for line in lines {
        let Some((name, value)) = line.split_once(':') else {
            continue;
        };
        if name.eq_ignore_ascii_case("Sec-WebSocket-Key") {
            key = Some(value.trim().to_owned());
        } else if name.eq_ignore_ascii_case("Upgrade")
            && value.trim().eq_ignore_ascii_case("websocket")
        {
            upgrade = true;
        }
    }
    if !upgrade {
        anyhow::bail!("missing WebSocket Upgrade header");
    }
    Ok((path, key.context("missing Sec-WebSocket-Key")?))
}

fn websocket_accept(key: &str) -> String {
    let mut sha1 = Sha1::new();
    sha1.update(key.as_bytes());
    sha1.update(b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
    STANDARD.encode(sha1.finalize())
}

fn websocket_loop(
    connection: &mut Connection,
    source: Source,
    state: &AppState,
    shutdown: &AtomicBool,
) -> Result<()> {
    let mut received = Vec::with_capacity(4096);
    let mut buffer = [0u8; 4096];
    while !shutdown.load(Ordering::Acquire) {
        let outgoing = match &source {
            Source::Live(subscription)
            | Source::Audio(subscription)
            | Source::Logs(subscription) => subscription.recv_timeout(Duration::from_millis(5)),
            Source::Talk => {
                thread::sleep(Duration::from_millis(5));
                None
            }
        };
        if let Some(outgoing) = outgoing {
            send_frame(connection, 0x2, &outgoing)?;
        }

        loop {
            match connection.read_some(&mut buffer)? {
                Some(0) => return Ok(()),
                Some(size) => {
                    received.extend_from_slice(&buffer[..size]);
                    if received.len() > MAX_CLIENT_PAYLOAD + 14 {
                        anyhow::bail!("WebSocket receive buffer exceeded limit");
                    }
                }
                None => break,
            }
        }

        while let Some((opcode, payload, consumed)) = parse_frame(&received)? {
            received.drain(..consumed);
            match opcode {
                0x2 if matches!(source, Source::Talk) => {
                    state.talk_feed(&payload);
                }
                0x8 => {
                    let _ = send_frame(connection, 0x8, &[]);
                    return Ok(());
                }
                0x9 => send_frame(connection, 0xA, &payload)?,
                0xA => {}
                _ => {}
            }
        }
    }
    Ok(())
}

fn parse_frame(data: &[u8]) -> Result<Option<(u8, Vec<u8>, usize)>> {
    if data.len() < 2 {
        return Ok(None);
    }
    let fin = data[0] & 0x80 != 0;
    let opcode = data[0] & 0x0f;
    let masked = data[1] & 0x80 != 0;
    if !fin || !masked {
        anyhow::bail!("fragmented or unmasked client WebSocket frame");
    }
    let mut length = usize::from(data[1] & 0x7f);
    let mut header = 2;
    if length == 126 {
        if data.len() < 4 {
            return Ok(None);
        }
        length = usize::from(u16::from_be_bytes([data[2], data[3]]));
        header = 4;
    } else if length == 127 {
        if data.len() < 10 {
            return Ok(None);
        }
        let value = u64::from_be_bytes(data[2..10].try_into().unwrap());
        length = usize::try_from(value).context("WebSocket payload length overflow")?;
        header = 10;
    }
    if length > MAX_CLIENT_PAYLOAD || (opcode >= 0x8 && length > 125) {
        anyhow::bail!("WebSocket payload exceeds limit");
    }
    if data.len() < header + 4 || length > data.len().saturating_sub(header + 4) {
        return Ok(None);
    }
    let mask = &data[header..header + 4];
    let payload_start = header + 4;
    let mut payload = data[payload_start..payload_start + length].to_vec();
    for (index, byte) in payload.iter_mut().enumerate() {
        *byte ^= mask[index % 4];
    }
    Ok(Some((opcode, payload, payload_start + length)))
}

fn send_frame(connection: &mut Connection, opcode: u8, payload: &[u8]) -> Result<()> {
    let mut header = Vec::with_capacity(10);
    header.push(0x80 | opcode);
    match payload.len() {
        length @ 0..=125 => header.push(length as u8),
        length @ 126..=65535 => {
            header.push(126);
            header.extend_from_slice(&(length as u16).to_be_bytes());
        }
        length => {
            header.push(127);
            header.extend_from_slice(&(length as u64).to_be_bytes());
        }
    }
    connection.write_all_timeout(&header, WRITE_TIMEOUT)?;
    if !payload.is_empty() {
        connection.write_all_timeout(payload, WRITE_TIMEOUT)?;
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn websocket_accept_matches_rfc_example() {
        assert_eq!(
            websocket_accept("dGhlIHNhbXBsZSBub25jZQ=="),
            "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="
        );
    }

    #[test]
    fn parses_masked_binary_frame() {
        let frame = [0x82, 0x83, 1, 2, 3, 4, b'a' ^ 1, b'b' ^ 2, b'c' ^ 3];
        let (_, payload, consumed) = parse_frame(&frame).unwrap().unwrap();
        assert_eq!(payload, b"abc");
        assert_eq!(consumed, frame.len());
    }
}
