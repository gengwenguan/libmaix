use crate::native::NativeEngine;
use anyhow::{bail, Context, Result};
use std::fs::File;
use std::io::{Read, Take};
use std::path::PathBuf;
use std::sync::Arc;

const MAX_WAV_BYTES: u64 = 2 * 1024 * 1024;
const PCM_EXTENSIBLE_GUID: [u8; 16] = [
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71,
];

pub struct PromptService {
    directory: PathBuf,
    engine: Arc<NativeEngine>,
}

impl PromptService {
    pub fn new(directory: PathBuf, engine: Arc<NativeEngine>) -> Self {
        Self { directory, engine }
    }

    pub fn play(&self, name: &str) -> Result<()> {
        if !Self::valid_name(name) {
            bail!("invalid prompt name");
        }
        let path = self.directory.join(format!("{name}.wav"));
        let bytes = read_bounded(&path)?;
        let samples = parse_wav_48k_mono_s16(&bytes)?;
        drop(bytes);
        self.engine.play_prompt_pcm(&samples)
    }

    pub fn valid_name(name: &str) -> bool {
        !name.is_empty()
            && name.len() <= 64
            && name
                .bytes()
                .all(|byte| byte.is_ascii_alphanumeric() || byte == b'_' || byte == b'-')
    }
}

fn read_bounded(path: &std::path::Path) -> Result<Vec<u8>> {
    let file = File::open(path).with_context(|| format!("open prompt {}", path.display()))?;
    let size = file.metadata()?.len();
    if size == 0 || size > MAX_WAV_BYTES {
        bail!("prompt file size is invalid");
    }
    let mut bytes = Vec::with_capacity(size as usize);
    let mut limited: Take<File> = file.take(MAX_WAV_BYTES + 1);
    limited.read_to_end(&mut bytes)?;
    if bytes.len() as u64 != size || bytes.len() as u64 > MAX_WAV_BYTES {
        bail!("prompt file changed while reading");
    }
    Ok(bytes)
}

fn parse_wav_48k_mono_s16(bytes: &[u8]) -> Result<Vec<i16>> {
    if bytes.len() < 12 || &bytes[..4] != b"RIFF" || &bytes[8..12] != b"WAVE" {
        bail!("prompt is not a RIFF/WAVE file");
    }

    let mut format_ok = false;
    let mut pcm = None;
    let mut offset = 12usize;
    while offset.checked_add(8).is_some_and(|end| end <= bytes.len()) {
        let id = &bytes[offset..offset + 4];
        let size = read_u32_le(bytes, offset + 4)? as usize;
        let data_start = offset + 8;
        let data_end = data_start
            .checked_add(size)
            .context("WAV chunk size overflow")?;
        if data_end > bytes.len() {
            bail!("WAV chunk exceeds file size");
        }

        if id == b"fmt " {
            format_ok = parse_format(&bytes[data_start..data_end])?;
        } else if id == b"data" && pcm.is_none() {
            pcm = Some(&bytes[data_start..data_end]);
        }
        offset = data_end
            .checked_add(size & 1)
            .context("WAV chunk padding overflow")?;
    }

    let pcm = pcm.context("WAV data chunk missing")?;
    if !format_ok {
        bail!("prompt must be PCM 48kHz mono S16_LE");
    }
    if pcm.is_empty() || pcm.len() % 2 != 0 {
        bail!("prompt PCM data length is invalid");
    }
    Ok(pcm
        .chunks_exact(2)
        .map(|sample| i16::from_le_bytes([sample[0], sample[1]]))
        .collect())
}

fn parse_format(format: &[u8]) -> Result<bool> {
    if format.len() < 16 {
        bail!("WAV fmt chunk is too short");
    }
    let tag = u16::from_le_bytes([format[0], format[1]]);
    let channels = u16::from_le_bytes([format[2], format[3]]);
    let sample_rate = u32::from_le_bytes(format[4..8].try_into().unwrap());
    let bits = u16::from_le_bytes([format[14], format[15]]);
    let pcm_format =
        tag == 1 || (tag == 0xfffe && format.len() >= 40 && format[24..40] == PCM_EXTENSIBLE_GUID);
    Ok(pcm_format && channels == 1 && sample_rate == 48_000 && bits == 16)
}

fn read_u32_le(bytes: &[u8], offset: usize) -> Result<u32> {
    let value = bytes
        .get(offset..offset + 4)
        .context("truncated WAV integer")?;
    Ok(u32::from_le_bytes(value.try_into().unwrap()))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn wav(samples: &[i16], sample_rate: u32, channels: u16) -> Vec<u8> {
        let data: Vec<u8> = samples
            .iter()
            .flat_map(|sample| sample.to_le_bytes())
            .collect();
        let mut bytes = Vec::new();
        bytes.extend_from_slice(b"RIFF");
        bytes.extend_from_slice(&(36 + data.len() as u32).to_le_bytes());
        bytes.extend_from_slice(b"WAVEfmt ");
        bytes.extend_from_slice(&16u32.to_le_bytes());
        bytes.extend_from_slice(&1u16.to_le_bytes());
        bytes.extend_from_slice(&channels.to_le_bytes());
        bytes.extend_from_slice(&sample_rate.to_le_bytes());
        bytes.extend_from_slice(&(sample_rate * u32::from(channels) * 2).to_le_bytes());
        bytes.extend_from_slice(&(channels * 2).to_le_bytes());
        bytes.extend_from_slice(&16u16.to_le_bytes());
        bytes.extend_from_slice(b"data");
        bytes.extend_from_slice(&(data.len() as u32).to_le_bytes());
        bytes.extend_from_slice(&data);
        bytes
    }

    #[test]
    fn parses_expected_pcm_format() {
        let samples = [-32768, -1, 0, 1, 32767];
        assert_eq!(
            parse_wav_48k_mono_s16(&wav(&samples, 48_000, 1)).unwrap(),
            samples
        );
    }

    #[test]
    fn rejects_wrong_sample_rate_and_traversal() {
        assert!(parse_wav_48k_mono_s16(&wav(&[0], 44_100, 1)).is_err());
        assert!(!PromptService::valid_name("../door"));
        assert!(PromptService::valid_name("door"));
    }
}
