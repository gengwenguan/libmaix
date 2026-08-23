use crate::native::NativeEngine;
use anyhow::{Context, Result};
use std::collections::VecDeque;
use std::sync::{Arc, Condvar, Mutex};
use std::thread::{self, JoinHandle};

const MAX_QUEUED_FRAMES: usize = 64;
const MAX_QUEUED_BYTES: usize = 256 * 1024;

struct QueueState {
    running: bool,
    bytes: usize,
    frames: VecDeque<Vec<u8>>,
}

struct FrameQueue {
    state: Mutex<QueueState>,
    ready: Condvar,
}

impl FrameQueue {
    fn new() -> Self {
        Self {
            state: Mutex::new(QueueState {
                running: false,
                bytes: 0,
                frames: VecDeque::new(),
            }),
            ready: Condvar::new(),
        }
    }

    fn start(&self) {
        let mut state = self.state.lock().unwrap_or_else(|error| error.into_inner());
        state.running = true;
        state.bytes = 0;
        state.frames.clear();
    }

    fn stop(&self) {
        let mut state = self.state.lock().unwrap_or_else(|error| error.into_inner());
        state.running = false;
        state.bytes = 0;
        state.frames.clear();
        self.ready.notify_all();
    }

    fn push(&self, frame: &[u8]) {
        if frame.is_empty() || frame.len() > MAX_QUEUED_BYTES {
            return;
        }
        let mut state = self.state.lock().unwrap_or_else(|error| error.into_inner());
        if !state.running {
            return;
        }
        while !state.frames.is_empty()
            && (state.frames.len() >= MAX_QUEUED_FRAMES
                || state.bytes + frame.len() > MAX_QUEUED_BYTES)
        {
            if let Some(dropped) = state.frames.pop_front() {
                state.bytes = state.bytes.saturating_sub(dropped.len());
            }
        }
        state.bytes += frame.len();
        state.frames.push_back(frame.to_vec());
        self.ready.notify_one();
    }

    fn pop(&self) -> Option<Vec<u8>> {
        let mut state = self.state.lock().unwrap_or_else(|error| error.into_inner());
        loop {
            if let Some(frame) = state.frames.pop_front() {
                state.bytes = state.bytes.saturating_sub(frame.len());
                return Some(frame);
            }
            if !state.running {
                return None;
            }
            state = self
                .ready
                .wait(state)
                .unwrap_or_else(|error| error.into_inner());
        }
    }
}

struct Lifecycle {
    clients: usize,
    worker: Option<JoinHandle<()>>,
}

pub struct TalkService {
    engine: Arc<NativeEngine>,
    queue: Arc<FrameQueue>,
    lifecycle: Mutex<Lifecycle>,
}

impl TalkService {
    pub fn new(engine: Arc<NativeEngine>) -> Self {
        Self {
            engine,
            queue: Arc::new(FrameQueue::new()),
            lifecycle: Mutex::new(Lifecycle {
                clients: 0,
                worker: None,
            }),
        }
    }

    pub fn connect(&self) -> Result<()> {
        let mut lifecycle = self
            .lifecycle
            .lock()
            .unwrap_or_else(|error| error.into_inner());
        if lifecycle.clients > 0 {
            lifecycle.clients += 1;
            return Ok(());
        }

        self.engine.talk_open()?;
        self.queue.start();
        let queue = self.queue.clone();
        let engine = self.engine.clone();
        let worker = thread::Builder::new()
            .name("talk-play".to_owned())
            .stack_size(512 * 1024)
            .spawn(move || {
                while let Some(frame) = queue.pop() {
                    if let Err(error) = engine.talk_play_frame(&frame) {
                        eprintln!("talk frame playback failed: {error:#}");
                    }
                }
            });
        match worker {
            Ok(worker) => {
                lifecycle.clients = 1;
                lifecycle.worker = Some(worker);
                Ok(())
            }
            Err(error) => {
                self.queue.stop();
                self.engine.talk_close();
                Err(error).context("spawn talk playback worker")
            }
        }
    }

    pub fn feed(&self, frame: &[u8]) {
        self.queue.push(frame);
    }

    pub fn disconnect(&self) {
        let mut lifecycle = self
            .lifecycle
            .lock()
            .unwrap_or_else(|error| error.into_inner());
        if lifecycle.clients == 0 {
            return;
        }
        lifecycle.clients -= 1;
        if lifecycle.clients > 0 {
            return;
        }

        self.queue.stop();
        if let Some(worker) = lifecycle.worker.take() {
            let _ = worker.join();
        }
        self.engine.talk_close();
    }
}

impl Drop for TalkService {
    fn drop(&mut self) {
        self.queue.stop();
        let lifecycle = self
            .lifecycle
            .get_mut()
            .unwrap_or_else(|error| error.into_inner());
        if let Some(worker) = lifecycle.worker.take() {
            let _ = worker.join();
        }
        if lifecycle.clients > 0 {
            self.engine.talk_close();
            lifecycle.clients = 0;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn queue_drops_oldest_frame_at_limit() {
        let queue = FrameQueue::new();
        queue.start();
        for value in 0..=MAX_QUEUED_FRAMES {
            queue.push(&[value as u8]);
        }
        assert_eq!(queue.pop(), Some(vec![1]));
        queue.stop();
    }

    #[test]
    fn queue_rejects_single_oversized_frame() {
        let queue = FrameQueue::new();
        queue.start();
        queue.push(&vec![0; MAX_QUEUED_BYTES + 1]);
        queue.stop();
        assert_eq!(queue.pop(), None);
    }
}
