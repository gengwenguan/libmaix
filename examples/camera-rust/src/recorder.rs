use crate::config::RuntimeConfig;
use anyhow::{Context, Result};
use chrono::{Duration as ChronoDuration, Utc};
use serde::Serialize;
use std::collections::BTreeMap;
use std::fs::{self, File, OpenOptions};
use std::io::{BufWriter, Write};
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

#[derive(Clone, Serialize)]
pub struct RecordStatus {
    pub enabled: bool,
    pub recording: bool,
    pub file: String,
    pub bytes: u64,
    pub root: String,
}

struct State {
    init: Arc<[u8]>,
    file: Option<BufWriter<File>>,
    index: Option<BufWriter<File>>,
    index_first: bool,
    current: PathBuf,
    bytes: u64,
    started: Instant,
    tfdt_base: BTreeMap<u32, u64>,
}

pub struct Recorder {
    root: PathBuf,
    config: Arc<RuntimeConfig>,
    state: Mutex<State>,
}

impl Recorder {
    pub fn new(root: PathBuf, config: Arc<RuntimeConfig>) -> Result<Self> {
        fs::create_dir_all(&root)
            .with_context(|| format!("create record directory {}", root.display()))?;
        Ok(Self {
            root,
            config,
            state: Mutex::new(State {
                init: Arc::from([]),
                file: None,
                index: None,
                index_first: true,
                current: PathBuf::new(),
                bytes: 0,
                started: Instant::now(),
                tfdt_base: BTreeMap::new(),
            }),
        })
    }

    pub fn set_init_segment(&self, data: &[u8]) {
        self.state
            .lock()
            .unwrap_or_else(|error| error.into_inner())
            .init = Arc::from(data);
    }

    pub fn write_fragment(&self, data: &[u8]) -> Result<()> {
        if data.is_empty() {
            return Ok(());
        }
        let mut state = self.state.lock().unwrap_or_else(|error| error.into_inner());
        if !self.config.bool("record_enabled", true) {
            close_recording(&mut state)?;
            return Ok(());
        }
        if state.init.is_empty() {
            return Ok(());
        }
        let segment_secs = self.config.int("record_segment_s", 600).clamp(10, 3600) as u64;
        if state.file.is_none() || state.started.elapsed() >= Duration::from_secs(segment_secs) {
            self.roll_locked(&mut state)?;
        }

        let mut fragment = data.to_vec();
        let tfdt = rewrite_tfdt(&mut fragment, &mut state.tfdt_base).unwrap_or(0);
        let offset = state.bytes;
        let file = state.file.as_mut().context("record file is not open")?;
        file.write_all(&fragment).context("write MP4 fragment")?;
        file.flush().context("flush MP4 fragment")?;
        state.bytes = state.bytes.saturating_add(fragment.len() as u64);

        let index_first = state.index_first;
        state.index_first = false;
        if let Some(index) = state.index.as_mut() {
            if !index_first {
                index.write_all(b",")?;
            }
            write!(index, "[{tfdt},{offset},{}]", fragment.len())?;
            index.flush()?;
        }
        Ok(())
    }

    pub fn status(&self) -> RecordStatus {
        let state = self.state.lock().unwrap_or_else(|error| error.into_inner());
        RecordStatus {
            enabled: self.config.bool("record_enabled", true),
            recording: state.file.is_some(),
            file: state
                .current
                .file_name()
                .map(|name| name.to_string_lossy().into_owned())
                .unwrap_or_default(),
            bytes: state.bytes,
            root: self.root.to_string_lossy().into_owned(),
        }
    }

    pub fn root(&self) -> &Path {
        &self.root
    }

    pub fn current_path(&self) -> PathBuf {
        self.state
            .lock()
            .unwrap_or_else(|error| error.into_inner())
            .current
            .clone()
    }

    fn roll_locked(&self, state: &mut State) -> Result<()> {
        close_index(state)?;
        if let Some(mut file) = state.file.take() {
            file.flush()?;
        }

        let china = Utc::now() + ChronoDuration::hours(8);
        let day = china.format("%Y%m%d").to_string();
        let name = china.format("%Y%m%d_%H%M%S.mp4").to_string();
        let directory = self.root.join(day);
        fs::create_dir_all(&directory)?;
        let path = directory.join(name);

        let file =
            File::create(&path).with_context(|| format!("create record {}", path.display()))?;
        let mut writer = BufWriter::with_capacity(64 * 1024, file);
        writer.write_all(&state.init)?;

        let index_path = PathBuf::from(format!("{}.idx", path.display()));
        let index_file = OpenOptions::new()
            .create(true)
            .write(true)
            .truncate(true)
            .open(&index_path)?;
        let mut index = BufWriter::new(index_file);
        write!(
            index,
            "{{\"v\":1,\"ts\":90000,\"init\":{},\"frags\":[",
            state.init.len()
        )?;
        index.flush()?;

        state.file = Some(writer);
        state.index = Some(index);
        state.index_first = true;
        state.current = path;
        state.bytes = state.init.len() as u64;
        state.started = Instant::now();
        state.tfdt_base.clear();
        Ok(())
    }
}

impl Drop for Recorder {
    fn drop(&mut self) {
        if let Ok(state) = self.state.get_mut() {
            let _ = close_index(state);
            if let Some(file) = state.file.as_mut() {
                let _ = file.flush();
            }
        }
    }
}

fn close_index(state: &mut State) -> Result<()> {
    if let Some(mut index) = state.index.take() {
        index.write_all(b"]}\n")?;
        index.flush()?;
    }
    Ok(())
}

fn close_recording(state: &mut State) -> Result<()> {
    close_index(state)?;
    if let Some(mut file) = state.file.take() {
        file.flush()?;
    }
    Ok(())
}

fn read_u32(data: &[u8], offset: usize) -> Option<u32> {
    Some(u32::from_be_bytes(
        data.get(offset..offset + 4)?.try_into().ok()?,
    ))
}

fn read_u64(data: &[u8], offset: usize) -> Option<u64> {
    Some(u64::from_be_bytes(
        data.get(offset..offset + 8)?.try_into().ok()?,
    ))
}

fn box_header(data: &[u8], offset: usize, end: usize) -> Option<(usize, [u8; 4], usize)> {
    let short = read_u32(data, offset)? as usize;
    let kind = data.get(offset + 4..offset + 8)?.try_into().ok()?;
    let (size, header) = if short == 1 {
        (usize::try_from(read_u64(data, offset + 8)?).ok()?, 16)
    } else {
        (short, 8)
    };
    if size < header || offset.checked_add(size)? > end {
        return None;
    }
    Some((size, kind, header))
}

fn rewrite_tfdt(data: &mut [u8], base: &mut BTreeMap<u32, u64>) -> Option<u64> {
    let mut video_tfdt = None;
    let mut position = 0;
    while position + 8 <= data.len() {
        let (size, kind, header) = box_header(data, position, data.len())?;
        if &kind == b"moof" {
            let mut child = position + header;
            let end = position + size;
            while child + 8 <= end {
                let (child_size, child_kind, child_header) = box_header(data, child, end)?;
                if &child_kind == b"traf" {
                    rewrite_traf(
                        data,
                        child + child_header,
                        child + child_size,
                        base,
                        &mut video_tfdt,
                    );
                }
                child += child_size;
            }
        }
        position += size;
    }
    video_tfdt
}

fn rewrite_traf(
    data: &mut [u8],
    start: usize,
    end: usize,
    bases: &mut BTreeMap<u32, u64>,
    video_tfdt: &mut Option<u64>,
) {
    let mut track_id = None;
    let mut position = start;
    while position + 8 <= end {
        let Some((size, kind, header)) = box_header(data, position, end) else {
            return;
        };
        if &kind == b"tfhd" {
            track_id = read_u32(data, position + header + 4);
            break;
        }
        position += size;
    }
    let Some(track_id) = track_id else { return };

    position = start;
    while position + 8 <= end {
        let Some((size, kind, header)) = box_header(data, position, end) else {
            return;
        };
        if &kind == b"tfdt" {
            let Some(&version) = data.get(position + header) else {
                return;
            };
            let value_offset = position + header + 4;
            let original = if version == 1 {
                read_u64(data, value_offset)
            } else {
                read_u32(data, value_offset).map(u64::from)
            };
            let Some(original) = original else { return };
            let anchor = *bases.entry(track_id).or_insert(original);
            let fixed = original.saturating_sub(anchor);
            if version == 1 {
                if let Some(target) = data.get_mut(value_offset..value_offset + 8) {
                    target.copy_from_slice(&fixed.to_be_bytes());
                }
            } else if let Some(target) = data.get_mut(value_offset..value_offset + 4) {
                target.copy_from_slice(&(fixed as u32).to_be_bytes());
            }
            if track_id == 1 && video_tfdt.is_none() {
                *video_tfdt = Some(fixed);
            }
            return;
        }
        position += size;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn make_fragment(track: u32, tfdt: u64) -> Vec<u8> {
        let mut tfhd = Vec::new();
        tfhd.extend_from_slice(&16u32.to_be_bytes());
        tfhd.extend_from_slice(b"tfhd");
        tfhd.extend_from_slice(&[0, 0, 0, 0]);
        tfhd.extend_from_slice(&track.to_be_bytes());

        let mut tfdt_box = Vec::new();
        tfdt_box.extend_from_slice(&20u32.to_be_bytes());
        tfdt_box.extend_from_slice(b"tfdt");
        tfdt_box.extend_from_slice(&[1, 0, 0, 0]);
        tfdt_box.extend_from_slice(&tfdt.to_be_bytes());

        let traf_size = 8 + tfhd.len() + tfdt_box.len();
        let mut traf = Vec::new();
        traf.extend_from_slice(&(traf_size as u32).to_be_bytes());
        traf.extend_from_slice(b"traf");
        traf.extend_from_slice(&tfhd);
        traf.extend_from_slice(&tfdt_box);

        let moof_size = 8 + traf.len();
        let mut moof = Vec::new();
        moof.extend_from_slice(&(moof_size as u32).to_be_bytes());
        moof.extend_from_slice(b"moof");
        moof.extend_from_slice(&traf);
        moof
    }

    #[test]
    fn tfdt_is_rebased_per_track() {
        let mut bases = BTreeMap::new();
        let mut first = make_fragment(1, 900_000);
        let mut second = make_fragment(1, 990_000);
        assert_eq!(rewrite_tfdt(&mut first, &mut bases), Some(0));
        assert_eq!(rewrite_tfdt(&mut second, &mut bases), Some(90_000));
    }
}
