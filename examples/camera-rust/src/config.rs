use anyhow::{Context, Result};
use serde::{Deserialize, Serialize};
use serde_json::{Map, Value};
use std::fs::{self, File};
use std::io::Write;
use std::path::{Path, PathBuf};
use std::sync::RwLock;

#[derive(Clone, Debug, Deserialize, PartialEq, Serialize)]
#[serde(default)]
pub struct AppConfig {
    pub ai_enabled: bool,
    pub ai_threshold: f64,
    pub ai_min_interval_s: i32,
    pub ai_infer_fps: i32,
    pub record_enabled: bool,
    pub record_segment_s: i32,
    pub record_retain_days: i32,
    pub record_max_bytes: u64,
    pub album_max_photos: i32,
    pub photo_jpeg_qual: i32,
    pub mic_filter_mode: i32,
    pub vmd_enabled: bool,
    pub vmd_pixel_thresh: i32,
    pub vmd_area_ratio: f64,
    pub vmd_min_interval_s: i32,
    pub vmd_check_fps: i32,
    pub osd_show_ip: bool,
    pub osd_show_time: bool,
    pub osd_show_ai_box: bool,
    pub light_enabled: bool,
    pub light_mode: i32,
    pub light_start_hour: i32,
    pub light_end_hour: i32,
    pub light_sound_thresh: i32,
    pub light_hold_s: i32,
    pub light_gpio: i32,
    pub light_active_low: bool,
    pub mqtt_enabled: bool,
    pub mqtt_broker_host: String,
    pub mqtt_broker_port: i32,
    pub mqtt_topic: String,
    pub mqtt_client_id: String,
    pub mqtt_poll_sec: i32,
    pub mqtt_iface: String,
    pub mqtt_report_interval_s: i32,
    pub mqtt_retain: bool,
    pub camera_hub_url: String,
    pub camera_hub_follow_board_prefix: bool,
    pub camera_hub_device_id: String,
}

impl Default for AppConfig {
    fn default() -> Self {
        Self {
            ai_enabled: false,
            ai_threshold: 0.5,
            ai_min_interval_s: 3,
            ai_infer_fps: 1,
            record_enabled: true,
            record_segment_s: 600,
            record_retain_days: 7,
            record_max_bytes: 16 * 1024 * 1024 * 1024,
            album_max_photos: 1000,
            photo_jpeg_qual: 88,
            mic_filter_mode: 5,
            vmd_enabled: false,
            vmd_pixel_thresh: 25,
            vmd_area_ratio: 0.021,
            vmd_min_interval_s: 2,
            vmd_check_fps: 5,
            osd_show_ip: true,
            osd_show_time: true,
            osd_show_ai_box: true,
            light_enabled: true,
            light_mode: 1,
            light_start_hour: 18,
            light_end_hour: 6,
            light_sound_thresh: 35,
            light_hold_s: 30,
            light_gpio: 237,
            light_active_low: false,
            mqtt_enabled: true,
            mqtt_broker_host: "broker.emqx.io".to_owned(),
            mqtt_broker_port: 1883,
            mqtt_topic: "geng-cam-ipv6".to_owned(),
            mqtt_client_id: "v831cam".to_owned(),
            mqtt_poll_sec: 10,
            mqtt_iface: "wlan0".to_owned(),
            mqtt_report_interval_s: 3600,
            mqtt_retain: true,
            camera_hub_url: "http://mi6.gwghome.site".to_owned(),
            camera_hub_follow_board_prefix: false,
            camera_hub_device_id: "v831cam".to_owned(),
        }
    }
}

impl AppConfig {
    fn normalize(&mut self) {
        self.ai_threshold = self.ai_threshold.clamp(0.1, 0.95);
        self.ai_min_interval_s = self.ai_min_interval_s.clamp(1, 3600);
        self.ai_infer_fps = self.ai_infer_fps.clamp(1, 30);
        self.record_segment_s = self.record_segment_s.clamp(10, 3600);
        self.record_retain_days = self.record_retain_days.clamp(1, 365);
        self.record_max_bytes = self
            .record_max_bytes
            .clamp(64 * 1024 * 1024, 512 * 1024 * 1024 * 1024);
        self.album_max_photos = self.album_max_photos.clamp(10, 100_000);
        self.photo_jpeg_qual = self.photo_jpeg_qual.clamp(30, 100);
        self.mic_filter_mode = self.mic_filter_mode.clamp(0, 5);
        self.vmd_pixel_thresh = self.vmd_pixel_thresh.clamp(1, 255);
        self.vmd_area_ratio = self.vmd_area_ratio.clamp(0.001, 0.5);
        self.vmd_min_interval_s = self.vmd_min_interval_s.clamp(1, 3600);
        self.vmd_check_fps = self.vmd_check_fps.clamp(1, 30);
        self.light_mode = self.light_mode.clamp(0, 1);
        self.light_start_hour = self.light_start_hour.clamp(0, 23);
        self.light_end_hour = self.light_end_hour.clamp(0, 23);
        self.light_sound_thresh = self.light_sound_thresh.clamp(0, 100);
        self.light_hold_s = self.light_hold_s.clamp(1, 3600);
        if !(0..=287).contains(&self.light_gpio) {
            self.light_gpio = 237;
        }
        self.mqtt_broker_port = self.mqtt_broker_port.clamp(1, 65535);
        self.mqtt_poll_sec = self.mqtt_poll_sec.clamp(2, 3600);
        if self.mqtt_report_interval_s != 0 {
            self.mqtt_report_interval_s = self.mqtt_report_interval_s.clamp(60, 86400);
        }
        self.camera_hub_url = self.camera_hub_url.trim().chars().take(512).collect();
        self.camera_hub_device_id = self
            .camera_hub_device_id
            .trim()
            .chars()
            .filter(|character| character.is_ascii_alphanumeric() || "-_.".contains(*character))
            .take(64)
            .collect();
        if self.camera_hub_device_id.is_empty() {
            self.camera_hub_device_id = "v831cam".to_owned();
        }
    }
}

#[derive(Default, Deserialize)]
#[serde(default)]
struct ConfigPatch {
    ai_enabled: Option<bool>,
    ai_threshold: Option<f64>,
    ai_min_interval_s: Option<i32>,
    ai_infer_fps: Option<i32>,
    record_enabled: Option<bool>,
    record_segment_s: Option<i32>,
    record_retain_days: Option<i32>,
    record_max_bytes: Option<u64>,
    album_max_photos: Option<i32>,
    photo_jpeg_qual: Option<i32>,
    mic_filter_mode: Option<i32>,
    vmd_enabled: Option<bool>,
    vmd_pixel_thresh: Option<i32>,
    vmd_area_ratio: Option<f64>,
    vmd_min_interval_s: Option<i32>,
    vmd_check_fps: Option<i32>,
    osd_show_ip: Option<bool>,
    osd_show_time: Option<bool>,
    osd_show_ai_box: Option<bool>,
    light_enabled: Option<bool>,
    light_mode: Option<i32>,
    light_start_hour: Option<i32>,
    light_end_hour: Option<i32>,
    light_sound_thresh: Option<i32>,
    light_hold_s: Option<i32>,
    light_gpio: Option<i32>,
    light_active_low: Option<bool>,
    mqtt_enabled: Option<bool>,
    mqtt_broker_host: Option<String>,
    mqtt_broker_port: Option<i32>,
    mqtt_topic: Option<String>,
    mqtt_client_id: Option<String>,
    mqtt_poll_sec: Option<i32>,
    mqtt_iface: Option<String>,
    mqtt_report_interval_s: Option<i32>,
    mqtt_retain: Option<bool>,
    camera_hub_url: Option<String>,
    camera_hub_follow_board_prefix: Option<bool>,
    camera_hub_device_id: Option<String>,
}

impl ConfigPatch {
    fn apply(self, config: &mut AppConfig) {
        macro_rules! apply {
            ($($field:ident),+ $(,)?) => {
                $(if let Some(value) = self.$field {
                    config.$field = value;
                })+
            };
        }
        apply!(
            ai_enabled,
            ai_threshold,
            ai_min_interval_s,
            ai_infer_fps,
            record_enabled,
            record_segment_s,
            record_retain_days,
            record_max_bytes,
            album_max_photos,
            photo_jpeg_qual,
            mic_filter_mode,
            vmd_enabled,
            vmd_pixel_thresh,
            vmd_area_ratio,
            vmd_min_interval_s,
            vmd_check_fps,
            osd_show_ip,
            osd_show_time,
            osd_show_ai_box,
            light_enabled,
            light_mode,
            light_start_hour,
            light_end_hour,
            light_sound_thresh,
            light_hold_s,
            light_gpio,
            light_active_low,
            mqtt_enabled,
            mqtt_broker_host,
            mqtt_broker_port,
            mqtt_topic,
            mqtt_client_id,
            mqtt_poll_sec,
            mqtt_iface,
            mqtt_report_interval_s,
            mqtt_retain,
            camera_hub_url,
            camera_hub_follow_board_prefix,
            camera_hub_device_id,
        );
    }
}

pub struct RuntimeConfig {
    path: PathBuf,
    value: RwLock<AppConfig>,
}

#[derive(Clone, Copy)]
pub struct VmdConfig {
    pub enabled: bool,
    pub pixel_threshold: u8,
    pub area_ratio: f64,
    pub minimum_interval: u64,
    pub check_fps: u32,
}

impl RuntimeConfig {
    #[cfg(test)]
    pub fn for_test(value: AppConfig) -> Self {
        Self {
            path: PathBuf::new(),
            value: RwLock::new(value),
        }
    }

    pub fn load(path: PathBuf) -> Result<Self> {
        let mut needs_save = false;
        let mut config = match fs::read_to_string(&path) {
            Ok(text) => match serde_json::from_str::<Value>(&text) {
                Ok(raw) => match serde_json::from_value::<AppConfig>(raw) {
                    Ok(config) => config,
                    Err(error) => {
                        eprintln!("config parse failed, using defaults: {error}");
                        needs_save = true;
                        AppConfig::default()
                    }
                },
                Err(error) => {
                    eprintln!("config JSON failed, using defaults: {error}");
                    needs_save = true;
                    AppConfig::default()
                }
            },
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => {
                needs_save = true;
                AppConfig::default()
            }
            Err(error) => return Err(error).context("read config"),
        };
        let before = serde_json::to_vec(&config)?;
        config.normalize();
        needs_save |= before != serde_json::to_vec(&config)?;

        let runtime = Self {
            path,
            value: RwLock::new(config),
        };
        if needs_save {
            runtime.save_current()?;
        }
        Ok(runtime)
    }

    pub fn current(&self) -> AppConfig {
        self.value
            .read()
            .unwrap_or_else(|error| error.into_inner())
            .clone()
    }

    pub fn vmd(&self) -> VmdConfig {
        let value = self.value.read().unwrap_or_else(|error| error.into_inner());
        VmdConfig {
            enabled: value.vmd_enabled,
            pixel_threshold: value.vmd_pixel_thresh as u8,
            area_ratio: value.vmd_area_ratio,
            minimum_interval: value.vmd_min_interval_s as u64,
            check_fps: value.vmd_check_fps as u32,
        }
    }

    pub fn snapshot(&self) -> Value {
        serde_json::to_value(self.current()).unwrap_or_else(|_| Value::Object(Map::new()))
    }

    pub fn apply_patch(&self, patch: Map<String, Value>) -> Result<AppConfig> {
        let patch = serde_json::from_value::<ConfigPatch>(Value::Object(patch))
            .context("invalid config patch")?;
        let mut value = self
            .value
            .write()
            .unwrap_or_else(|error| error.into_inner());
        let mut updated = value.clone();
        patch.apply(&mut updated);
        updated.normalize();
        save(&self.path, &updated)?;
        *value = updated.clone();
        Ok(updated)
    }

    pub fn int(&self, key: &str, default: i64) -> i64 {
        let value = self.value.read().unwrap_or_else(|error| error.into_inner());
        match key {
            "record_segment_s" => value.record_segment_s as i64,
            "record_retain_days" => value.record_retain_days as i64,
            "vmd_pixel_thresh" => value.vmd_pixel_thresh as i64,
            "vmd_min_interval_s" => value.vmd_min_interval_s as i64,
            "vmd_check_fps" => value.vmd_check_fps as i64,
            "light_mode" => value.light_mode as i64,
            "light_start_hour" => value.light_start_hour as i64,
            "light_end_hour" => value.light_end_hour as i64,
            "light_sound_thresh" => value.light_sound_thresh as i64,
            "light_hold_s" => value.light_hold_s as i64,
            "light_gpio" => value.light_gpio as i64,
            "mqtt_broker_port" => value.mqtt_broker_port as i64,
            "mqtt_poll_sec" => value.mqtt_poll_sec as i64,
            "mqtt_report_interval_s" => value.mqtt_report_interval_s as i64,
            _ => default,
        }
    }

    pub fn u64(&self, key: &str, default: u64) -> u64 {
        let value = self.value.read().unwrap_or_else(|error| error.into_inner());
        match key {
            "record_max_bytes" => value.record_max_bytes,
            _ => default,
        }
    }

    pub fn bool(&self, key: &str, default: bool) -> bool {
        let value = self.value.read().unwrap_or_else(|error| error.into_inner());
        match key {
            "record_enabled" => value.record_enabled,
            "vmd_enabled" => value.vmd_enabled,
            "light_enabled" => value.light_enabled,
            "light_active_low" => value.light_active_low,
            "mqtt_enabled" => value.mqtt_enabled,
            "mqtt_retain" => value.mqtt_retain,
            "camera_hub_follow_board_prefix" => value.camera_hub_follow_board_prefix,
            _ => default,
        }
    }

    pub fn string(&self, key: &str, default: &str) -> String {
        let value = self.value.read().unwrap_or_else(|error| error.into_inner());
        match key {
            "mqtt_broker_host" => value.mqtt_broker_host.clone(),
            "mqtt_topic" => value.mqtt_topic.clone(),
            "mqtt_client_id" => value.mqtt_client_id.clone(),
            "mqtt_iface" => value.mqtt_iface.clone(),
            "camera_hub_url" => value.camera_hub_url.clone(),
            "camera_hub_device_id" => value.camera_hub_device_id.clone(),
            _ => default.to_owned(),
        }
    }

    fn save_current(&self) -> Result<()> {
        let value = self.value.read().unwrap_or_else(|error| error.into_inner());
        save(&self.path, &value)
    }
}

fn save(path: &Path, config: &AppConfig) -> Result<()> {
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent)?;
    }
    let temporary = path.with_extension("json.tmp");
    let mut file = File::create(&temporary)?;
    serde_json::to_writer_pretty(&mut file, config)?;
    file.write_all(b"\n")?;
    file.sync_all()?;
    fs::rename(temporary, path)?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn patch_is_typed_and_clamped() {
        let mut config = AppConfig::default();
        let patch = serde_json::from_value::<ConfigPatch>(serde_json::json!({
            "ai_threshold": 2.0,
            "record_segment_s": 1,
            "unknown": 123
        }))
        .unwrap();
        patch.apply(&mut config);
        config.normalize();
        assert_eq!(config.ai_threshold, 0.95);
        assert_eq!(config.record_segment_s, 10);
    }

    #[test]
    fn defaults_match_tuned_device_profile() {
        let config = AppConfig::default();
        assert_eq!(config.ai_threshold, 0.5);
        assert_eq!(config.ai_min_interval_s, 3);
        assert_eq!(config.ai_infer_fps, 1);
        assert_eq!(config.mic_filter_mode, 5);
        assert_eq!(config.vmd_area_ratio, 0.021);
        assert!(config.light_enabled);
        assert_eq!(config.light_mode, 1);
        assert!(config.mqtt_enabled);
        assert_eq!(config.mqtt_topic, "geng-cam-ipv6");
    }

    #[test]
    fn camera_hub_fields_round_trip() {
        let config = AppConfig {
            camera_hub_url: "http://[::1]".to_owned(),
            camera_hub_follow_board_prefix: false,
            camera_hub_device_id: "front-door".to_owned(),
            ..AppConfig::default()
        };
        let encoded = serde_json::to_value(&config).unwrap();
        let decoded = serde_json::from_value::<AppConfig>(encoded).unwrap();
        assert_eq!(decoded.camera_hub_url, "http://[::1]");
        assert!(!decoded.camera_hub_follow_board_prefix);
        assert_eq!(decoded.camera_hub_device_id, "front-door");
    }
}
