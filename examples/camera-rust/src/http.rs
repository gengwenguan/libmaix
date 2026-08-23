use crate::actions::{ActionError, InvokeError};
use crate::app::AppState;
use crate::io::Connection;
use crate::native::TlsContext;
use crate::netinfo;
use crate::sysinfo::SysInfo;
use anyhow::{Context, Result};
use chrono::{Duration as ChronoDuration, Utc};
use serde::Serialize;
use serde_json::{json, Map, Value};
use std::collections::BTreeMap;
use std::fs::{self, File};
use std::io::{Read, Seek, SeekFrom};
use std::net::TcpListener;
use std::os::unix::fs::MetadataExt;
use std::path::{Component, Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant};

const MAX_HEADER_BYTES: usize = 16 * 1024;
const MAX_BODY_BYTES: usize = 1024 * 1024;
const REQUEST_TIMEOUT: Duration = Duration::from_secs(10);
const WRITE_TIMEOUT: Duration = Duration::from_secs(5);

pub struct HttpServers {
    shutdown: Arc<AtomicBool>,
    accept_threads: Vec<JoinHandle<()>>,
    clients: Arc<Mutex<Vec<JoinHandle<()>>>>,
}

impl HttpServers {
    pub fn start(state: Arc<AppState>, tls: Option<Arc<TlsContext>>) -> Result<Self> {
        let shutdown = Arc::new(AtomicBool::new(false));
        let clients = Arc::new(Mutex::new(Vec::new()));
        let active = Arc::new(AtomicUsize::new(0));
        let sysinfo = Arc::new(SysInfo::default());
        let mut accept_threads = Vec::new();

        for (port, listener_tls) in [(80, None), (443, tls)] {
            if port == 443 && listener_tls.is_none() {
                continue;
            }
            let listener =
                bind_dual_stack(port).with_context(|| format!("bind HTTP port {port}"))?;
            let state = state.clone();
            let shutdown_flag = shutdown.clone();
            let client_threads = clients.clone();
            let active_clients = active.clone();
            let sysinfo = sysinfo.clone();
            accept_threads.push(
                thread::Builder::new()
                    .name(format!("http-accept-{port}"))
                    .stack_size(192 * 1024)
                    .spawn(move || {
                        accept_loop(
                            listener,
                            listener_tls,
                            state,
                            sysinfo,
                            shutdown_flag,
                            client_threads,
                            active_clients,
                        )
                    })?,
            );
        }

        Ok(Self {
            shutdown,
            accept_threads,
            clients,
        })
    }
}

impl Drop for HttpServers {
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

#[allow(clippy::too_many_arguments)]
fn accept_loop(
    listener: TcpListener,
    tls: Option<Arc<TlsContext>>,
    state: Arc<AppState>,
    sysinfo: Arc<SysInfo>,
    shutdown: Arc<AtomicBool>,
    clients: Arc<Mutex<Vec<JoinHandle<()>>>>,
    active: Arc<AtomicUsize>,
) {
    while !shutdown.load(Ordering::Acquire) {
        match listener.accept() {
            Ok((stream, _)) => {
                if active.fetch_add(1, Ordering::AcqRel) >= 6 {
                    active.fetch_sub(1, Ordering::AcqRel);
                    continue;
                }
                let state = state.clone();
                let sysinfo = sysinfo.clone();
                let tls = tls.clone();
                let active_for_thread = active.clone();
                match thread::Builder::new()
                    .name("http-client".to_owned())
                    .stack_size(256 * 1024)
                    .spawn(move || {
                        let result = Connection::new(stream, tls)
                            .and_then(|connection| handle_client(connection, &state, &sysinfo));
                        if let Err(error) = result {
                            eprintln!("HTTP client failed: {error:#}");
                        }
                        active_for_thread.fetch_sub(1, Ordering::AcqRel);
                    }) {
                    Ok(handle) => clients
                        .lock()
                        .unwrap_or_else(|error| error.into_inner())
                        .push(handle),
                    Err(error) => {
                        active.fetch_sub(1, Ordering::AcqRel);
                        eprintln!("spawn HTTP client failed: {error}");
                    }
                }
            }
            Err(error) if error.kind() == std::io::ErrorKind::WouldBlock => {
                reap_clients(&clients);
                thread::sleep(Duration::from_millis(20));
            }
            Err(error) => {
                eprintln!("HTTP accept failed: {error}");
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

struct Request {
    method: String,
    path: String,
    query: String,
    headers: BTreeMap<String, String>,
    body: Vec<u8>,
}

struct Response {
    status: u16,
    content_type: &'static str,
    body: Vec<u8>,
}

enum Routed {
    Response(Response),
    File(PathBuf, &'static str),
}

impl Response {
    fn json(value: impl Serialize) -> Self {
        Self::json_status(200, value)
    }

    fn json_status(status: u16, value: impl Serialize) -> Self {
        match serde_json::to_vec(&value) {
            Ok(body) => Self {
                status,
                content_type: "application/json; charset=utf-8",
                body,
            },
            Err(error) => Self::text(500, format!("JSON error: {error}")),
        }
    }

    fn text(status: u16, text: impl Into<String>) -> Self {
        Self {
            status,
            content_type: "text/plain; charset=utf-8",
            body: text.into().into_bytes(),
        }
    }
}

fn handle_client(mut connection: Connection, state: &AppState, sysinfo: &SysInfo) -> Result<()> {
    let request = read_request(&mut connection)?;
    let route = route(&request, state, sysinfo);
    match route {
        Routed::Response(response) => send_response(&mut connection, response)?,
        Routed::File(path, content_type) => send_file(
            &mut connection,
            &path,
            content_type,
            request.headers.get("range"),
        )?,
    }
    connection.shutdown();
    Ok(())
}

fn read_request(connection: &mut Connection) -> Result<Request> {
    let mut data = connection.read_until(b"\r\n\r\n", MAX_HEADER_BYTES, REQUEST_TIMEOUT)?;
    let header_end = find_bytes(&data, b"\r\n\r\n").context("incomplete HTTP header")?;
    let header = std::str::from_utf8(&data[..header_end])
        .context("HTTP header is not UTF-8")?
        .to_owned();
    let mut lines = header.split("\r\n");
    let request_line = lines.next().context("missing HTTP request line")?;
    let mut request_parts = request_line.split_whitespace();
    let method = request_parts
        .next()
        .context("missing HTTP method")?
        .to_owned();
    let target = request_parts
        .next()
        .context("missing HTTP target")?
        .to_owned();
    let (path, query) = if let Some(separator) = target.find('?') {
        (
            target[..separator].to_owned(),
            target[separator + 1..].to_owned(),
        )
    } else {
        (target, String::new())
    };
    let mut headers = BTreeMap::new();
    for line in lines {
        if let Some((name, value)) = line.split_once(':') {
            headers.insert(name.trim().to_ascii_lowercase(), value.trim().to_owned());
        }
    }
    let content_length = headers
        .get("content-length")
        .map(|value| value.parse::<usize>())
        .transpose()
        .context("invalid Content-Length")?
        .unwrap_or(0);
    if content_length > MAX_BODY_BYTES {
        anyhow::bail!("HTTP body exceeds limit");
    }

    let body_start = header_end + 4;
    let deadline = Instant::now() + REQUEST_TIMEOUT;
    while data.len().saturating_sub(body_start) < content_length {
        if Instant::now() >= deadline {
            anyhow::bail!("HTTP body timeout");
        }
        let mut buffer = [0u8; 4096];
        match connection.read_some(&mut buffer)? {
            Some(0) => anyhow::bail!("connection closed before HTTP body"),
            Some(size) => data.extend_from_slice(&buffer[..size]),
            None => thread::sleep(Duration::from_millis(2)),
        }
    }
    Ok(Request {
        method,
        path: percent_decode(&path),
        query,
        headers,
        body: data[body_start..body_start + content_length].to_vec(),
    })
}

fn route(request: &Request, state: &AppState, sysinfo: &SysInfo) -> Routed {
    let result = match (request.method.as_str(), request.path.as_str()) {
        ("GET", "/api/webrtc/status") => Routed::Response(Response::json(state.webrtc.status())),
        ("GET", "/api/camera-hub/status") => {
            Routed::Response(Response::json(state.remote_hub.status()))
        }
        ("POST", "/api/webrtc/offer") => {
            if !same_origin(request) {
                return Routed::Response(Response::text(403, "origin not allowed"));
            }
            let offer = match std::str::from_utf8(&request.body) {
                Ok(offer) => offer.to_owned(),
                Err(_) => return Routed::Response(Response::text(400, "offer SDP is not UTF-8")),
            };
            match state.webrtc.answer(offer) {
                Ok(answer) => Routed::Response(Response {
                    status: 201,
                    content_type: "application/sdp",
                    body: answer.into_bytes(),
                }),
                Err(error) => Routed::Response(Response::text(
                    400,
                    format!("WebRTC negotiation failed: {error:#}"),
                )),
            }
        }
        ("DELETE", "/api/webrtc/session") => {
            if !same_origin(request) {
                return Routed::Response(Response::text(403, "origin not allowed"));
            }
            state.webrtc.close();
            Routed::Response(Response::json(json!({"ok":true})))
        }
        ("GET", "/api/record/status") => Routed::Response(Response::json(state.recorder.status())),
        ("GET", "/api/record/days") => {
            Routed::Response(Response::json(record_days(state.recorder.root())))
        }
        ("GET", "/api/record/segments") => {
            let date = query_value(&request.query, "date").unwrap_or_else(today_china);
            match record_segments(state.recorder.root(), &date) {
                Ok(value) => Routed::Response(Response::json(value)),
                Err(error) => Routed::Response(Response::text(400, error.to_string())),
            }
        }
        ("POST", "/api/snapshot") => match state.snapshot.take_one(&state.engine) {
            Ok(name) => {
                let size = fs::metadata(state.snapshot_dir.join(&name))
                    .map(|metadata| metadata.len())
                    .unwrap_or(0);
                Routed::Response(Response::json(json!({"ok":true,"name":name,"size":size})))
            }
            Err(error) => {
                Routed::Response(Response::json(json!({"ok":false,"err":error.to_string()})))
            }
        },
        ("POST", "/api/prompt") => {
            let name = serde_json::from_slice::<Value>(&request.body)
                .ok()
                .and_then(|value| value.get("name")?.as_str().map(str::to_owned));
            match name {
                Some(name) if !crate::prompt::PromptService::valid_name(&name) => Routed::Response(
                    Response::json_status(400, json!({"ok":false,"err":"invalid name"})),
                ),
                Some(name) => match state.prompt.play(&name) {
                    Ok(()) => Routed::Response(Response::json(json!({"ok":true,"name":name}))),
                    Err(error) if error.to_string().contains("busy") => Routed::Response(
                        Response::json_status(409, json!({"ok":false,"err":"busy"})),
                    ),
                    Err(error) => Routed::Response(Response::json_status(
                        500,
                        json!({"ok":false,"err":error.to_string()}),
                    )),
                },
                None => Routed::Response(Response::json_status(
                    400,
                    json!({"ok":false,"err":"missing name"}),
                )),
            }
        }
        ("GET", "/api/photo/latest") => match photos(&state.snapshot_dir).into_iter().next() {
            Some(photo) => Routed::Response(Response::json(
                json!({"ok":true,"name":photo.name,"size":photo.size,"mtime":photo.mtime}),
            )),
            None => Routed::Response(Response::text(404, "no photo")),
        },
        ("GET", "/api/photo/list") => Routed::Response(Response::json(photos(&state.snapshot_dir))),
        ("POST", "/api/photo/delete") => {
            Routed::Response(delete_photos(&state.snapshot_dir, &request.body))
        }
        ("GET", "/api/config") => Routed::Response(Response::json(state.config.snapshot())),
        ("POST", "/api/config") => {
            let patch = parse_config_patch(request);
            match patch.and_then(|patch| state.config.apply_patch(patch)) {
                Ok(config) => {
                    if let Err(error) = state.remote_hub.apply_config(&config) {
                        Routed::Response(Response::text(500, error.to_string()))
                    } else {
                        Routed::Response(Response::json(state.config.snapshot()))
                    }
                }
                Err(error) => Routed::Response(Response::text(400, error.to_string())),
            }
        }
        ("GET", "/api/netinfo") => {
            let interface = state.config.string("mqtt_iface", "wlan0");
            let (ipv4, ipv6) = netinfo::addresses(&interface);
            Routed::Response(Response::json(json!({"ipv4":ipv4,"ipv6":ipv6})))
        }
        ("GET", "/api/sysinfo") => {
            Routed::Response(Response::json(sysinfo.sample(state.recorder.root())))
        }
        ("GET", "/api/actions") => Routed::Response(Response::json(
            json!({"ok":true,"actions":state.actions.list()}),
        )),
        ("POST", "/api/actions") => {
            let value = serde_json::from_slice::<Value>(&request.body).unwrap_or(Value::Null);
            let result = state.actions.add(
                value.get("name").and_then(Value::as_str).unwrap_or(""),
                value.get("url").and_then(Value::as_str).unwrap_or(""),
            );
            Routed::Response(match result {
                Ok(action) => Response::json_status(201, json!({"ok":true,"action":action})),
                Err(error) => action_error_response(error),
            })
        }
        ("POST", "/api/actions/update") => {
            let value = serde_json::from_slice::<Value>(&request.body).unwrap_or(Value::Null);
            let result = state.actions.update(
                value.get("id").and_then(Value::as_str).unwrap_or(""),
                value.get("name").and_then(Value::as_str).unwrap_or(""),
                value.get("url").and_then(Value::as_str).unwrap_or(""),
            );
            Routed::Response(match result {
                Ok(action) => Response::json(json!({"ok":true,"action":action})),
                Err(error) => action_error_response(error),
            })
        }
        ("POST", "/api/actions/delete") => {
            let value = serde_json::from_slice::<Value>(&request.body).unwrap_or(Value::Null);
            let id = value.get("id").and_then(Value::as_str).unwrap_or("");
            Routed::Response(match state.actions.remove(id) {
                Ok(()) => Response::json(json!({"ok":true})),
                Err(error) => action_error_response(error),
            })
        }
        ("POST", "/api/actions/invoke") => {
            let value = serde_json::from_slice::<Value>(&request.body).unwrap_or(Value::Null);
            let id = value.get("id").and_then(Value::as_str).unwrap_or("");
            Routed::Response(match state.actions.invoke(id) {
                Ok((action, status)) => {
                    Response::json(json!({"ok":true,"status":status,"name":action.name}))
                }
                Err(InvokeError::Action(error)) => action_error_response(error),
                Err(InvokeError::Network(action, error)) => {
                    Response::json_status(502, json!({"ok":false,"err":error,"name":action.name}))
                }
            })
        }
        _ if request.method == "GET" && request.path.starts_with("/record/") => {
            match safe_record_path(state.recorder.root(), &request.path) {
                Some(path) if path.is_file() => Routed::File(
                    path.clone(),
                    if path.extension().is_some_and(|extension| extension == "idx") {
                        "application/json"
                    } else {
                        "video/mp4"
                    },
                ),
                _ => Routed::Response(Response::text(404, "not found")),
            }
        }
        _ if request.method == "GET" && request.path.starts_with("/photo/") => {
            let name = request.path.trim_start_matches("/photo/");
            match safe_file_name(name) {
                true if state.snapshot_dir.join(name).is_file() => {
                    Routed::File(state.snapshot_dir.join(name), "image/jpeg")
                }
                _ => Routed::Response(Response::text(404, "not found")),
            }
        }
        _ if request.method == "GET"
            && request
                .path
                .starts_with("/.well-known/acme-challenge/") =>
        {
            match acme_challenge_path(&state.runtime_dir, &request.path) {
                Some(path) if path.is_file() => Routed::File(path, "text/plain; charset=utf-8"),
                _ => Routed::Response(Response::text(404, "challenge not found")),
            }
        }
        ("GET", path) => match static_path(&state.web_dir, path) {
            Some(path) if path.is_file() => Routed::File(path.clone(), mime_type(&path)),
            _ => Routed::Response(Response::text(404, "not found")),
        },
        _ => Routed::Response(Response::text(404, "not found")),
    };
    result
}

fn send_response(connection: &mut Connection, response: Response) -> Result<()> {
    let header = format!(
        "HTTP/1.1 {} {}\r\n\
         Content-Type: {}\r\n\
         Content-Length: {}\r\n\
         Connection: close\r\n\
         Cache-Control: no-cache, no-store, must-revalidate\r\n\r\n",
        response.status,
        status_text(response.status),
        response.content_type,
        response.body.len()
    );
    connection.write_all_timeout(header.as_bytes(), WRITE_TIMEOUT)?;
    connection.write_all_timeout(&response.body, WRITE_TIMEOUT)
}

fn send_file(
    connection: &mut Connection,
    path: &Path,
    content_type: &str,
    range: Option<&String>,
) -> Result<()> {
    let mut file = File::open(path)?;
    let size = file.metadata()?.len();
    let parsed_range = range.and_then(|value| parse_range(value, size));
    if range.is_some() && parsed_range.is_none() {
        return send_response(connection, Response::text(416, "invalid range"));
    }
    let (first, last, status) = parsed_range
        .map(|(first, last)| (first, last, 206))
        .unwrap_or((0, size.saturating_sub(1), 200));
    let content_length = if size == 0 { 0 } else { last - first + 1 };
    let mut header = format!(
        "HTTP/1.1 {status} {}\r\n\
         Content-Type: {content_type}\r\n\
         Content-Length: {content_length}\r\n\
         Accept-Ranges: bytes\r\n\
         Connection: close\r\n",
        status_text(status)
    );
    if status == 206 {
        header.push_str(&format!("Content-Range: bytes {first}-{last}/{size}\r\n"));
    }
    header.push_str("\r\n");
    connection.write_all_timeout(header.as_bytes(), WRITE_TIMEOUT)?;
    if content_length == 0 {
        return Ok(());
    }
    file.seek(SeekFrom::Start(first))?;
    let mut remaining = content_length;
    let mut buffer = vec![0u8; 64 * 1024];
    while remaining > 0 {
        let chunk = remaining.min(buffer.len() as u64) as usize;
        let read = file.read(&mut buffer[..chunk])?;
        if read == 0 {
            break;
        }
        connection.write_all_timeout(&buffer[..read], WRITE_TIMEOUT)?;
        remaining -= read as u64;
    }
    Ok(())
}

fn status_text(status: u16) -> &'static str {
    match status {
        200 => "OK",
        201 => "Created",
        206 => "Partial Content",
        204 => "No Content",
        400 => "Bad Request",
        403 => "Forbidden",
        404 => "Not Found",
        409 => "Conflict",
        413 => "Payload Too Large",
        416 => "Range Not Satisfiable",
        500 => "Internal Server Error",
        502 => "Bad Gateway",
        503 => "Service Unavailable",
        _ => "Error",
    }
}

fn action_error_response(error: ActionError) -> Response {
    Response::json_status(
        error.http_status(),
        json!({"ok":false,"err":error.to_string()}),
    )
}

fn find_bytes(data: &[u8], needle: &[u8]) -> Option<usize> {
    data.windows(needle.len())
        .position(|window| window == needle)
}

fn same_origin(request: &Request) -> bool {
    let Some(origin) = request.headers.get("origin") else {
        return false;
    };
    let Some(host) = request.headers.get("host") else {
        return false;
    };
    origin == &format!("http://{host}") || origin == &format!("https://{host}")
}

fn parse_range(value: &str, size: u64) -> Option<(u64, u64)> {
    let value = value.trim().strip_prefix("bytes=")?;
    if value.contains(',') || size == 0 {
        return None;
    }
    let (first, last) = value.split_once('-')?;
    let first = first.parse::<u64>().ok()?;
    let last = if last.is_empty() {
        size - 1
    } else {
        last.parse::<u64>().ok()?.min(size - 1)
    };
    (first <= last && first < size).then_some((first, last))
}

fn static_path(root: &Path, path: &str) -> Option<PathBuf> {
    let path = if path == "/" {
        "index.html"
    } else {
        path.trim_start_matches('/')
    };
    safe_join(root, path)
}

fn safe_record_path(root: &Path, path: &str) -> Option<PathBuf> {
    safe_join(root, path.trim_start_matches("/record/"))
}

fn safe_join(root: &Path, relative: &str) -> Option<PathBuf> {
    let relative = Path::new(relative);
    if relative
        .components()
        .any(|component| !matches!(component, Component::Normal(_)))
    {
        return None;
    }
    Some(root.join(relative))
}

fn safe_file_name(name: &str) -> bool {
    !name.is_empty()
        && !name.contains("..")
        && !name.contains('/')
        && !name.contains('\\')
        && !name.chars().any(char::is_control)
}

fn acme_challenge_path(runtime_dir: &Path, request_path: &str) -> Option<PathBuf> {
    let token = request_path.strip_prefix("/.well-known/acme-challenge/")?;
    if token.is_empty()
        || token.len() > 256
        || !token
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'-' | b'_'))
    {
        return None;
    }
    Some(
        runtime_dir
            .join("state/acme-webroot/.well-known/acme-challenge")
            .join(token),
    )
}

fn mime_type(path: &Path) -> &'static str {
    match path
        .extension()
        .and_then(|value| value.to_str())
        .unwrap_or("")
    {
        "html" => "text/html; charset=utf-8",
        "css" => "text/css; charset=utf-8",
        "js" => "application/javascript; charset=utf-8",
        "svg" => "image/svg+xml",
        "ico" => "image/x-icon",
        "json" | "idx" => "application/json",
        "jpg" | "jpeg" => "image/jpeg",
        "mp4" => "video/mp4",
        _ => "application/octet-stream",
    }
}

fn query_value(query: &str, key: &str) -> Option<String> {
    query.split('&').find_map(|pair| {
        let (name, value) = pair.split_once('=')?;
        (name == key).then(|| percent_decode(value))
    })
}

fn percent_decode(value: &str) -> String {
    let bytes = value.as_bytes();
    let mut output = Vec::with_capacity(bytes.len());
    let mut index = 0;
    while index < bytes.len() {
        if bytes[index] == b'%' && index + 2 < bytes.len() {
            if let Ok(value) = u8::from_str_radix(&value[index + 1..index + 3], 16) {
                output.push(value);
                index += 3;
                continue;
            }
        }
        output.push(if bytes[index] == b'+' {
            b' '
        } else {
            bytes[index]
        });
        index += 1;
    }
    String::from_utf8_lossy(&output).into_owned()
}

fn parse_config_patch(request: &Request) -> Result<Map<String, Value>> {
    if let Ok(Value::Object(object)) = serde_json::from_slice::<Value>(&request.body) {
        return Ok(object);
    }
    let text = std::str::from_utf8(&request.body)?;
    let mut object = Map::new();
    for pair in text.split('&') {
        if let Some((key, value)) = pair.split_once('=') {
            object.insert(percent_decode(key), Value::String(percent_decode(value)));
        }
    }
    Ok(object)
}

fn today_china() -> String {
    (Utc::now() + ChronoDuration::hours(8))
        .format("%Y%m%d")
        .to_string()
}

fn record_days(root: &Path) -> Vec<String> {
    let mut days = fs::read_dir(root)
        .into_iter()
        .flatten()
        .flatten()
        .filter_map(|entry| {
            let name = entry.file_name().to_string_lossy().into_owned();
            (entry.path().is_dir()
                && name.len() == 8
                && name.chars().all(|character| character.is_ascii_digit()))
            .then_some(name)
        })
        .collect::<Vec<_>>();
    days.sort_by(|left, right| right.cmp(left));
    days
}

#[derive(Serialize)]
struct Segment {
    name: String,
    size: u64,
    hms: String,
    sec: u32,
}

fn record_segments(root: &Path, date: &str) -> Result<Vec<Segment>> {
    if date.len() != 8 || !date.chars().all(|character| character.is_ascii_digit()) {
        anyhow::bail!("bad date");
    }
    let mut segments = fs::read_dir(root.join(date))
        .into_iter()
        .flatten()
        .flatten()
        .filter_map(|entry| {
            let name = entry.file_name().to_string_lossy().into_owned();
            if !name.ends_with(".mp4") || name.len() < 19 {
                return None;
            }
            let hh = name.get(9..11)?.parse::<u32>().ok()?;
            let mm = name.get(11..13)?.parse::<u32>().ok()?;
            let ss = name.get(13..15)?.parse::<u32>().ok()?;
            let metadata = entry.metadata().ok()?;
            Some(Segment {
                name,
                size: metadata.len(),
                hms: format!("{hh:02}:{mm:02}:{ss:02}"),
                sec: hh * 3600 + mm * 60 + ss,
            })
        })
        .collect::<Vec<_>>();
    segments.sort_by(|left, right| left.name.cmp(&right.name));
    Ok(segments)
}

#[derive(Serialize)]
struct Photo {
    name: String,
    size: u64,
    mtime: i64,
}

fn photos(root: &Path) -> Vec<Photo> {
    let mut photos = fs::read_dir(root)
        .into_iter()
        .flatten()
        .flatten()
        .filter_map(|entry| {
            let name = entry.file_name().to_string_lossy().into_owned();
            if !safe_file_name(&name) || !name.ends_with(".jpg") {
                return None;
            }
            let metadata = entry.metadata().ok()?;
            Some(Photo {
                name,
                size: metadata.len(),
                mtime: metadata.mtime(),
            })
        })
        .collect::<Vec<_>>();
    photos.sort_by(|left, right| right.name.cmp(&left.name));
    photos
}

fn delete_photos(root: &Path, body: &[u8]) -> Response {
    let value = serde_json::from_slice::<Value>(body).unwrap_or(Value::Null);
    let names = value
        .get("names")
        .and_then(Value::as_array)
        .map(|names| {
            names
                .iter()
                .filter_map(Value::as_str)
                .map(str::to_owned)
                .collect::<Vec<_>>()
        })
        .or_else(|| {
            value
                .get("name")
                .and_then(Value::as_str)
                .map(|name| vec![name.to_owned()])
        })
        .unwrap_or_default();
    let mut deleted = 0;
    let mut missing = 0;
    let mut errors = Vec::new();
    for name in names {
        if !safe_file_name(&name) || !name.ends_with(".jpg") {
            errors.push(format!("{name}:bad-name"));
            continue;
        }
        match fs::remove_file(root.join(&name)) {
            Ok(()) => deleted += 1,
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => missing += 1,
            Err(_) => errors.push(format!("{name}:unlink-fail")),
        }
    }
    Response::json(json!({
        "ok": true,
        "deleted": deleted,
        "missing": missing,
        "errors": errors
    }))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn range_parser_rejects_invalid_ranges() {
        assert_eq!(parse_range("bytes=10-19", 100), Some((10, 19)));
        assert_eq!(parse_range("bytes=90-", 100), Some((90, 99)));
        assert_eq!(parse_range("bytes=100-", 100), None);
        assert_eq!(parse_range("bytes=1-2,4-5", 100), None);
    }

    #[test]
    fn safe_join_rejects_parent_components() {
        assert!(safe_join(Path::new("/tmp"), "web/app.js").is_some());
        assert!(safe_join(Path::new("/tmp"), "../secret").is_none());
    }

    #[test]
    fn acme_challenge_path_accepts_only_safe_tokens() {
        let root = Path::new("/root/maix_dist");
        assert_eq!(
            acme_challenge_path(root, "/.well-known/acme-challenge/abc_DEF-123"),
            Some(
                root.join("state/acme-webroot/.well-known/acme-challenge/abc_DEF-123")
            )
        );
        assert!(acme_challenge_path(root, "/.well-known/acme-challenge/../key").is_none());
        assert!(acme_challenge_path(root, "/.well-known/acme-challenge/").is_none());
    }

    #[test]
    fn webrtc_signaling_requires_same_origin() {
        let mut headers = BTreeMap::new();
        headers.insert("host".to_owned(), "[2409:8a1e:7a52:c9b0::1]".to_owned());
        headers.insert(
            "origin".to_owned(),
            "http://[2409:8a1e:7a52:c9b0::1]".to_owned(),
        );
        let request = Request {
            method: "POST".to_owned(),
            path: "/api/webrtc/offer".to_owned(),
            query: String::new(),
            headers,
            body: Vec::new(),
        };
        assert!(same_origin(&request));
    }
}
