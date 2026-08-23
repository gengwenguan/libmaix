use anyhow::{Context, Result};
use camera_rust::app::AppState;
use camera_rust::http::HttpServers;
use camera_rust::native::TlsContext;
use camera_rust::websocket::WebSocketServers;
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::thread;
use std::time::Duration;

static RUNNING: AtomicBool = AtomicBool::new(true);

extern "C" fn stop_signal(_: libc::c_int) {
    RUNNING.store(false, Ordering::Release);
}

fn main() {
    if let Err(error) = run() {
        eprintln!("camera-rust fatal: {error:#}");
        std::process::exit(1);
    }
}

fn run() -> Result<()> {
    eprintln!("[rust-stage] main entered");
    unsafe {
        libc::signal(libc::SIGPIPE, libc::SIG_IGN);
        libc::signal(libc::SIGINT, stop_signal as *const () as libc::sighandler_t);
        libc::signal(
            libc::SIGTERM,
            stop_signal as *const () as libc::sighandler_t,
        );
    }

    // Match the C++ service: give DHCP/NTP a short startup window before
    // naming files and rendering the first OSD timestamp.
    thread::sleep(Duration::from_secs(8));
    eprintln!("[rust-stage] startup delay complete");

    let runtime_dir = runtime_directory()?;
    let state = AppState::build(runtime_dir.clone())?;
    eprintln!("[rust-stage] application state ready");
    let tls = load_tls(&runtime_dir);

    let http = HttpServers::start(state.clone(), tls.clone())?;
    let websocket = WebSocketServers::start(state.clone(), tls)?;
    eprintln!("[rust-stage] network listeners ready");
    state.engine.start()?;
    eprintln!("[rust-stage] native media engine ready");

    eprintln!(
        "camera-rust started: HTTP=80 HTTPS=443 WS=8081/8082 WSS=8444/8445 runtime={}",
        runtime_dir.display()
    );
    while RUNNING.load(Ordering::Acquire) {
        thread::sleep(Duration::from_millis(250));
    }

    // Stop external entry points before releasing media callbacks and devices.
    drop(http);
    drop(websocket);
    state.webrtc.close();
    state.engine.stop();
    eprintln!("camera-rust stopped");
    Ok(())
}

fn runtime_directory() -> Result<PathBuf> {
    let executable = std::env::current_exe().context("resolve /proc/self/exe")?;
    executable
        .parent()
        .map(PathBuf::from)
        .context("executable has no parent directory")
}

fn load_tls(runtime_dir: &std::path::Path) -> Option<Arc<TlsContext>> {
    let managed_cert = runtime_dir.join("state/tls/fullchain.pem");
    let managed_key = runtime_dir.join("state/tls/private.key");
    let legacy_cert = runtime_dir.join("cert/server.crt");
    let legacy_key = runtime_dir.join("cert/server.key");
    let (cert, key) = if managed_cert.is_file() && managed_key.is_file() {
        (managed_cert, managed_key)
    } else {
        (legacy_cert, legacy_key)
    };
    if !cert.is_file() || !key.is_file() {
        eprintln!("TLS certificate missing; HTTPS/WSS disabled");
        return None;
    }
    match TlsContext::new(&cert.to_string_lossy(), &key.to_string_lossy()) {
        Ok(context) => Some(Arc::new(context)),
        Err(error) => {
            eprintln!("TLS init failed; HTTPS/WSS disabled: {error:#}");
            None
        }
    }
}
