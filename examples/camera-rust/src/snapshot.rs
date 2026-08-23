use crate::config::RuntimeConfig;
use crate::native::NativeEngine;
use anyhow::{Context, Result};
use chrono::{Datelike, Duration as ChronoDuration, Timelike, Utc};
use std::fs::{self, File};
use std::io::Write;
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Condvar, Mutex};
use std::time::Duration;

const FRAME_BYTES: usize = 640 * 480 * 3 / 2;
const MAX_IMPORTED_JPEG_BYTES: usize = 1024 * 1024;

struct FrameState {
    frame: Option<Vec<u8>>,
}

struct Sequence {
    day: i32,
    value: u32,
}

pub struct SnapshotService {
    directory: PathBuf,
    config: Arc<RuntimeConfig>,
    want_frame: AtomicBool,
    frame: Mutex<FrameState>,
    frame_ready: Condvar,
    capture_lock: Mutex<()>,
    sequence: Mutex<Sequence>,
}

impl SnapshotService {
    pub fn new(directory: PathBuf, config: Arc<RuntimeConfig>) -> Result<Arc<Self>> {
        fs::create_dir_all(&directory)
            .with_context(|| format!("create snapshot directory {}", directory.display()))?;
        Ok(Arc::new(Self {
            directory,
            config,
            want_frame: AtomicBool::new(false),
            frame: Mutex::new(FrameState { frame: None }),
            frame_ready: Condvar::new(),
            capture_lock: Mutex::new(()),
            sequence: Mutex::new(Sequence { day: 0, value: 0 }),
        }))
    }

    pub fn on_nv21_frame(&self, frame: &[u8]) {
        if frame.len() < FRAME_BYTES || !self.want_frame.load(Ordering::Relaxed) {
            return;
        }
        let mut state = self.frame.lock().unwrap_or_else(|error| error.into_inner());
        if !self.want_frame.swap(false, Ordering::Relaxed) {
            return;
        }
        state.frame = Some(frame[..FRAME_BYTES].to_vec());
        self.frame_ready.notify_one();
    }

    pub fn take_one(&self, engine: &NativeEngine) -> Result<String> {
        let _capture = self
            .capture_lock
            .lock()
            .unwrap_or_else(|error| error.into_inner());
        let frame = {
            let mut state = self.frame.lock().unwrap_or_else(|error| error.into_inner());
            state.frame = None;
            self.want_frame.store(true, Ordering::Relaxed);
            let (mut state, timeout) = self
                .frame_ready
                .wait_timeout_while(state, Duration::from_secs(1), |state| state.frame.is_none())
                .unwrap_or_else(|error| error.into_inner());
            if timeout.timed_out() {
                self.want_frame.store(false, Ordering::Relaxed);
                anyhow::bail!("snapshot timed out waiting for frame");
            }
            state.frame.take().context("snapshot frame missing")?
        };

        let config = self.config.current();
        let jpeg = engine.encode_jpeg(&frame, config.photo_jpeg_qual)?;
        let china = Utc::now() + ChronoDuration::hours(8);
        let day = china.year() * 10000 + china.month() as i32 * 100 + china.day() as i32;
        let sequence = {
            let mut sequence = self
                .sequence
                .lock()
                .unwrap_or_else(|error| error.into_inner());
            if sequence.day != day {
                sequence.day = day;
                sequence.value = 0;
            }
            sequence.value = sequence.value.saturating_add(1);
            sequence.value
        };
        let name = format!(
            "{:04}{:02}{:02}_{:02}{:02}{:02}_{sequence:03}.jpg",
            china.year(),
            china.month(),
            china.day(),
            china.hour(),
            china.minute(),
            china.second(),
        );
        let path = self.directory.join(&name);
        let temporary = self.directory.join(format!("{name}.tmp"));
        let mut file = File::create(&temporary)?;
        file.write_all(&jpeg)?;
        file.sync_all()?;
        fs::rename(temporary, &path)?;
        self.prune(config.album_max_photos)?;
        Ok(name)
    }

    pub fn import_camera_hub_jpeg(&self, source_name: &str, jpeg: &[u8]) -> Result<String> {
        if !valid_camera_hub_source_name(source_name)
            || jpeg.len() < 4
            || jpeg.len() > MAX_IMPORTED_JPEG_BYTES
            || !jpeg.starts_with(&[0xff, 0xd8])
            || !jpeg.ends_with(&[0xff, 0xd9])
        {
            anyhow::bail!("invalid camera-hub snapshot");
        }
        let name = format!("camera_hub_{source_name}");
        let path = self.directory.join(&name);
        if path.is_file() {
            return Ok(name);
        }
        let temporary = self.directory.join(format!("{name}.tmp"));
        let mut file = File::create(&temporary)?;
        file.write_all(jpeg)?;
        file.sync_all()?;
        fs::rename(temporary, &path)?;
        self.prune(self.config.current().album_max_photos)?;
        Ok(name)
    }

    pub fn latest_camera_hub_source_name(&self) -> String {
        fs::read_dir(&self.directory)
            .into_iter()
            .flatten()
            .flatten()
            .filter_map(|entry| {
                let name = entry.file_name().to_string_lossy().into_owned();
                name.strip_prefix("camera_hub_")
                    .filter(|source| valid_camera_hub_source_name(source))
                    .map(str::to_owned)
            })
            .max()
            .unwrap_or_default()
    }

    fn prune(&self, maximum: i32) -> Result<()> {
        let maximum = maximum.max(1) as usize;
        let mut photos = fs::read_dir(&self.directory)?
            .flatten()
            .filter_map(|entry| {
                let path = entry.path();
                if path.extension().is_none_or(|extension| extension != "jpg") {
                    return None;
                }
                let modified = entry.metadata().ok()?.modified().ok()?;
                Some((modified, path))
            })
            .collect::<Vec<_>>();
        if photos.len() <= maximum {
            return Ok(());
        }
        photos.sort_by_key(|(modified, _)| *modified);
        let remove = photos.len() - maximum;
        for (_, path) in photos.into_iter().take(remove) {
            fs::remove_file(path)?;
        }
        Ok(())
    }
}

fn valid_camera_hub_source_name(name: &str) -> bool {
    name.ends_with(".jpg")
        && name.len() <= 64
        && name
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'_' | b'-' | b'.'))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn frame_fast_path_does_not_allocate_without_request() {
        let config = Arc::new(RuntimeConfig::for_test(Default::default()));
        let root = std::env::temp_dir().join("camera-rust-snapshot-test");
        let service = SnapshotService::new(root.clone(), config).unwrap();
        service.on_nv21_frame(&vec![0; FRAME_BYTES]);
        let state = service.frame.lock().unwrap();
        assert!(state.frame.is_none());
        let _ = fs::remove_dir_all(root);
    }

    #[test]
    fn imports_camera_hub_snapshot_atomically() {
        let config = Arc::new(RuntimeConfig::for_test(Default::default()));
        let root = std::env::temp_dir().join("camera-rust-camera-hub-snapshot-test");
        let _ = fs::remove_dir_all(&root);
        let service = SnapshotService::new(root.clone(), config).unwrap();
        let jpeg = [0xff, 0xd8, 1, 2, 0xff, 0xd9];
        let source = "20260811_010203_004.jpg";
        assert_eq!(
            service.import_camera_hub_jpeg(source, &jpeg).unwrap(),
            format!("camera_hub_{source}")
        );
        assert_eq!(service.latest_camera_hub_source_name(), source);
        assert_eq!(
            fs::read(root.join(format!("camera_hub_{source}"))).unwrap(),
            jpeg
        );
        let _ = fs::remove_dir_all(root);
    }
}
