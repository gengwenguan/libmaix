use crate::config::RuntimeConfig;
use crate::native::NativeEngine;
use crate::snapshot::SnapshotService;
use anyhow::{Context, Result};
use std::sync::mpsc::{self, Receiver, SyncSender, TryRecvError, TrySendError};
use std::sync::{Arc, Mutex};
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant};

const WIDTH: usize = 640;
const HEIGHT: usize = 480;
const DOWN_WIDTH: usize = 80;
const DOWN_HEIGHT: usize = 60;
const DOWN_PIXELS: usize = DOWN_WIDTH * DOWN_HEIGHT;

struct MotionState {
    previous: Box<[u8; DOWN_PIXELS]>,
    have_reference: bool,
    last_check: Option<Instant>,
    last_ratio: f64,
}

pub struct MotionInput {
    config: Arc<RuntimeConfig>,
    trigger: SyncSender<()>,
    state: Mutex<MotionState>,
}

impl MotionInput {
    pub fn channel(config: Arc<RuntimeConfig>) -> (Arc<Self>, Receiver<()>) {
        let (trigger, receiver) = mpsc::sync_channel(1);
        (
            Arc::new(Self {
                config,
                trigger,
                state: Mutex::new(MotionState {
                    previous: Box::new([0; DOWN_PIXELS]),
                    have_reference: false,
                    last_check: None,
                    last_ratio: 0.0,
                }),
            }),
            receiver,
        )
    }

    pub fn input_y_plane(&self, y_plane: &[u8]) {
        if y_plane.len() < WIDTH * HEIGHT {
            return;
        }
        let config = self.config.vmd();
        let mut state = self.state.lock().unwrap_or_else(|error| error.into_inner());
        if !config.enabled {
            state.have_reference = false;
            state.last_ratio = 0.0;
            return;
        }

        let now = Instant::now();
        let period = Duration::from_millis(1000 / u64::from(config.check_fps.max(1)));
        if state
            .last_check
            .is_some_and(|last| now.duration_since(last) < period)
        {
            return;
        }
        state.last_check = Some(now);

        let mut current = [0u8; DOWN_PIXELS];
        let stride_x = WIDTH / DOWN_WIDTH;
        let stride_y = HEIGHT / DOWN_HEIGHT;
        for y in 0..DOWN_HEIGHT {
            let source_row = y * stride_y * WIDTH;
            let target_row = y * DOWN_WIDTH;
            for x in 0..DOWN_WIDTH {
                current[target_row + x] = y_plane[source_row + x * stride_x];
            }
        }

        if !state.have_reference {
            state.previous.copy_from_slice(&current);
            state.have_reference = true;
            return;
        }
        let changed = current
            .iter()
            .zip(state.previous.iter())
            .filter(|(current, previous)| (**current).abs_diff(**previous) > config.pixel_threshold)
            .count();
        state.previous.copy_from_slice(&current);
        state.last_ratio = changed as f64 / DOWN_PIXELS as f64;
        if state.last_ratio > config.area_ratio {
            match self.trigger.try_send(()) {
                Ok(()) | Err(TrySendError::Full(())) => {}
                Err(TrySendError::Disconnected(())) => {}
            }
        }
    }
}

struct StopSignal {
    sender: SyncSender<()>,
}

pub struct MotionWorker {
    stop: Option<StopSignal>,
    thread: Option<JoinHandle<()>>,
}

impl MotionWorker {
    pub fn start(
        receiver: Receiver<()>,
        config: Arc<RuntimeConfig>,
        snapshot: Arc<SnapshotService>,
        engine: Arc<NativeEngine>,
    ) -> Result<Self> {
        let (stop_sender, stop_receiver) = mpsc::sync_channel(1);
        let thread = thread::Builder::new()
            .name("motion-worker".to_owned())
            .stack_size(512 * 1024)
            .spawn(move || worker_loop(receiver, stop_receiver, config, snapshot, engine))
            .context("spawn motion worker")?;
        Ok(Self {
            stop: Some(StopSignal {
                sender: stop_sender,
            }),
            thread: Some(thread),
        })
    }
}

impl Drop for MotionWorker {
    fn drop(&mut self) {
        if let Some(stop) = self.stop.take() {
            let _ = stop.sender.try_send(());
        }
        if let Some(thread) = self.thread.take() {
            let _ = thread.join();
        }
    }
}

fn worker_loop(
    receiver: Receiver<()>,
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
            Ok(()) => {
                let minimum = Duration::from_secs(config.vmd().minimum_interval);
                if last_trigger.is_some_and(|last| last.elapsed() < minimum) {
                    continue;
                }
                last_trigger = Some(Instant::now());
                match snapshot.take_one(&engine) {
                    Ok(name) => eprintln!("motion detected, snapshot -> {name}"),
                    Err(error) => eprintln!("motion snapshot failed: {error:#}"),
                }
            }
            Err(mpsc::RecvTimeoutError::Timeout) => {}
            Err(mpsc::RecvTimeoutError::Disconnected) => return,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn downsampled_difference_detects_large_motion() {
        let config = Arc::new(RuntimeConfig::for_test(AppConfigForTest::enabled()));
        let (input, receiver) = MotionInput::channel(config);
        let first = vec![0u8; WIDTH * HEIGHT];
        let second = vec![255u8; WIDTH * HEIGHT];
        input.input_y_plane(&first);
        std::thread::sleep(Duration::from_millis(35));
        input.input_y_plane(&second);
        assert!(receiver.try_recv().is_ok());
    }

    // Keeps production constructors private while avoiding filesystem state.
    struct AppConfigForTest;

    impl AppConfigForTest {
        fn enabled() -> crate::config::AppConfig {
            crate::config::AppConfig {
                vmd_enabled: true,
                vmd_check_fps: 30,
                ..crate::config::AppConfig::default()
            }
        }
    }
}
