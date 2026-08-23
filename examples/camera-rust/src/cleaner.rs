use crate::config::RuntimeConfig;
use crate::recorder::Recorder;
use anyhow::{Context, Result};
use chrono::{Duration as ChronoDuration, NaiveDate, Utc};
use std::fs;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Condvar, Mutex};
use std::thread::{self, JoinHandle};
use std::time::Duration;

const SWEEP_INTERVAL: Duration = Duration::from_secs(3600);

struct StopSignal {
    stopped: Mutex<bool>,
    wake: Condvar,
}

pub struct RecordCleaner {
    stop: Arc<StopSignal>,
    thread: Option<JoinHandle<()>>,
}

impl RecordCleaner {
    pub fn start(
        root: PathBuf,
        config: Arc<RuntimeConfig>,
        recorder: Arc<Recorder>,
    ) -> Result<Self> {
        let stop = Arc::new(StopSignal {
            stopped: Mutex::new(false),
            wake: Condvar::new(),
        });
        let thread_stop = stop.clone();
        let thread = thread::Builder::new()
            .name("record-cleaner".to_owned())
            .stack_size(160 * 1024)
            .spawn(move || cleaner_loop(&root, &config, &recorder, thread_stop))
            .context("spawn record cleaner")?;
        Ok(Self {
            stop,
            thread: Some(thread),
        })
    }
}

impl Drop for RecordCleaner {
    fn drop(&mut self) {
        *self
            .stop
            .stopped
            .lock()
            .unwrap_or_else(|error| error.into_inner()) = true;
        self.stop.wake.notify_all();
        if let Some(thread) = self.thread.take() {
            let _ = thread.join();
        }
    }
}

fn cleaner_loop(root: &Path, config: &RuntimeConfig, recorder: &Recorder, stop: Arc<StopSignal>) {
    while !is_stopped(&stop) {
        if let Err(error) = sweep(root, config, recorder) {
            eprintln!("record cleaner failed: {error:#}");
        }
        let stopped = stop
            .stopped
            .lock()
            .unwrap_or_else(|error| error.into_inner());
        let _ = stop
            .wake
            .wait_timeout_while(stopped, SWEEP_INTERVAL, |stopped| !*stopped)
            .unwrap_or_else(|error| error.into_inner());
    }
}

fn is_stopped(stop: &StopSignal) -> bool {
    *stop
        .stopped
        .lock()
        .unwrap_or_else(|error| error.into_inner())
}

fn sweep(root: &Path, config: &RuntimeConfig, recorder: &Recorder) -> Result<()> {
    fs::create_dir_all(root)?;
    sweep_expired(root, config.int("record_retain_days", 7).clamp(1, 365))?;
    sweep_quota(
        root,
        config
            .u64("record_max_bytes", 16 * 1024 * 1024 * 1024)
            .clamp(64 * 1024 * 1024, 512 * 1024 * 1024 * 1024),
        &recorder.current_path(),
    )
}

fn sweep_expired(root: &Path, retain_days: i64) -> Result<()> {
    let today = (Utc::now() + ChronoDuration::hours(8)).date_naive();
    let cutoff = today - ChronoDuration::days(retain_days);
    for entry in fs::read_dir(root)? {
        let entry = entry?;
        if !entry.path().is_dir() {
            continue;
        }
        let name = entry.file_name();
        let Some(name) = name.to_str() else { continue };
        let Ok(day) = NaiveDate::parse_from_str(name, "%Y%m%d") else {
            continue;
        };
        if day < cutoff {
            fs::remove_dir_all(entry.path())?;
            eprintln!("record cleaner removed expired day {name}");
        }
    }
    Ok(())
}

fn sweep_quota(root: &Path, maximum: u64, current: &Path) -> Result<()> {
    let mut files = collect_files(root)?;
    let mut total = files.iter().map(|(_, size)| *size).sum::<u64>();
    if total <= maximum {
        return Ok(());
    }
    files.sort_by(|left, right| left.0.cmp(&right.0));
    let target = maximum.saturating_mul(9) / 10;
    let current_index = PathBuf::from(format!("{}.idx", current.display()));
    for (path, size) in files {
        if total <= target {
            break;
        }
        if path == current || path == current_index || !path.exists() {
            continue;
        }
        fs::remove_file(&path)?;
        total = total.saturating_sub(size);
        if path.extension().is_some_and(|extension| extension == "mp4") {
            let index = PathBuf::from(format!("{}.idx", path.display()));
            if let Ok(metadata) = fs::metadata(&index) {
                fs::remove_file(index)?;
                total = total.saturating_sub(metadata.len());
            }
        }
        eprintln!("record cleaner removed {}", path.display());
    }
    Ok(())
}

fn collect_files(root: &Path) -> Result<Vec<(PathBuf, u64)>> {
    let mut files = Vec::new();
    for day in fs::read_dir(root)? {
        let day = day?;
        if !day.path().is_dir() {
            continue;
        }
        for entry in fs::read_dir(day.path())? {
            let entry = entry?;
            let metadata = entry.metadata()?;
            if metadata.is_file() {
                files.push((entry.path(), metadata.len()));
            }
        }
    }
    Ok(files)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn date_parser_rejects_non_day_directories() {
        assert!(NaiveDate::parse_from_str("20260808", "%Y%m%d").is_ok());
        assert!(NaiveDate::parse_from_str("snapshot", "%Y%m%d").is_err());
    }
}
