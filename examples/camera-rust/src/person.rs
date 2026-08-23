use crate::config::RuntimeConfig;
use crate::native::NativeEngine;
use crate::snapshot::SnapshotService;
use anyhow::{Context, Result};
use std::sync::mpsc::{self, Receiver, SyncSender, TryRecvError, TrySendError};
use std::sync::Arc;
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant};

pub struct PersonInput {
    sender: SyncSender<f32>,
}

impl PersonInput {
    pub fn channel() -> (Arc<Self>, Receiver<f32>) {
        let (sender, receiver) = mpsc::sync_channel(1);
        (Arc::new(Self { sender }), receiver)
    }

    pub fn detected(&self, probability: f32) {
        match self.sender.try_send(probability) {
            Ok(()) | Err(TrySendError::Full(_)) | Err(TrySendError::Disconnected(_)) => {}
        }
    }
}

pub struct PersonWorker {
    stop: Option<SyncSender<()>>,
    thread: Option<JoinHandle<()>>,
}

impl PersonWorker {
    pub fn start(
        receiver: Receiver<f32>,
        config: Arc<RuntimeConfig>,
        snapshot: Arc<SnapshotService>,
        engine: Arc<NativeEngine>,
    ) -> Result<Self> {
        let (stop, stop_receiver) = mpsc::sync_channel(1);
        let thread = thread::Builder::new()
            .name("person-worker".to_owned())
            .stack_size(512 * 1024)
            .spawn(move || worker_loop(receiver, stop_receiver, config, snapshot, engine))
            .context("spawn person worker")?;
        Ok(Self {
            stop: Some(stop),
            thread: Some(thread),
        })
    }
}

impl Drop for PersonWorker {
    fn drop(&mut self) {
        if let Some(stop) = self.stop.take() {
            let _ = stop.try_send(());
        }
        if let Some(thread) = self.thread.take() {
            let _ = thread.join();
        }
    }
}

fn worker_loop(
    receiver: Receiver<f32>,
    stop: Receiver<()>,
    config: Arc<RuntimeConfig>,
    snapshot: Arc<SnapshotService>,
    engine: Arc<NativeEngine>,
) {
    let mut last_trigger = None::<Instant>;
    loop {
        match stop.try_recv() {
            Ok(()) | Err(TryRecvError::Disconnected) => return,
            Err(TryRecvError::Empty) => {}
        }
        match receiver.recv_timeout(Duration::from_millis(500)) {
            Ok(probability) => {
                let current = config.current();
                let minimum = Duration::from_secs(current.ai_min_interval_s as u64);
                if last_trigger.is_some_and(|last| last.elapsed() < minimum) {
                    continue;
                }
                last_trigger = Some(Instant::now());
                match snapshot.take_one(&engine) {
                    Ok(name) => {
                        eprintln!("person detected ({probability:.2}), snapshot -> {name}")
                    }
                    Err(error) => eprintln!("person snapshot failed: {error:#}"),
                }
            }
            Err(mpsc::RecvTimeoutError::Timeout) => {}
            Err(mpsc::RecvTimeoutError::Disconnected) => return,
        }
    }
}
