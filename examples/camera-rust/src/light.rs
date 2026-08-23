use crate::config::RuntimeConfig;
use crate::native::NativeEngine;
use anyhow::{Context, Result};
use chrono::{Duration as ChronoDuration, Timelike, Utc};
use std::fs::{File, OpenOptions};
use std::io::Write;
use std::path::PathBuf;
use std::sync::{Arc, Condvar, Mutex};
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant};

const TICK: Duration = Duration::from_millis(500);

struct StopSignal {
    stopped: Mutex<bool>,
    wake: Condvar,
}

pub struct LightController {
    stop: Arc<StopSignal>,
    thread: Option<JoinHandle<()>>,
}

impl LightController {
    pub fn start(config: Arc<RuntimeConfig>, engine: Arc<NativeEngine>) -> Result<Self> {
        let stop = Arc::new(StopSignal {
            stopped: Mutex::new(false),
            wake: Condvar::new(),
        });
        let thread_stop = stop.clone();
        let thread = thread::Builder::new()
            .name("light-controller".to_owned())
            .stack_size(128 * 1024)
            .spawn(move || light_loop(config, engine, thread_stop))
            .context("spawn light controller")?;
        Ok(Self {
            stop,
            thread: Some(thread),
        })
    }
}

impl Drop for LightController {
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

fn light_loop(config: Arc<RuntimeConfig>, engine: Arc<NativeEngine>, stop: Arc<StopSignal>) {
    let mut gpio = None::<Gpio>;
    let mut sound_hold_until = None::<Instant>;
    while !is_stopped(&stop) {
        let gpio_number = config.int("light_gpio", 237).clamp(0, 287) as i32;
        let active_low = config.bool("light_active_low", false);
        if gpio.as_ref().is_none_or(|gpio| gpio.number != gpio_number) {
            gpio = match Gpio::open(gpio_number, active_low) {
                Ok(gpio) => {
                    eprintln!(
                        "light controller: gpio{} ready (active_{})",
                        gpio_number,
                        if active_low { "low" } else { "high" }
                    );
                    Some(gpio)
                }
                Err(error) => {
                    eprintln!("light controller: GPIO init failed: {error:#}");
                    None
                }
            };
        }
        if let Some(gpio) = gpio.as_mut() {
            gpio.set_active_low(active_low);
            let enabled = config.bool("light_enabled", false);
            let start = config.int("light_start_hour", 18).clamp(0, 23) as u32;
            let end = config.int("light_end_hour", 6).clamp(0, 23) as u32;
            let china_hour = (Utc::now() + ChronoDuration::hours(8)).hour();
            let inside_window = hour_in_window(china_hour, start, end);
            let mode = config.int("light_mode", 0).clamp(0, 1);
            let on = if !enabled || !inside_window {
                false
            } else if mode == 0 {
                true
            } else {
                let threshold = config.int("light_sound_thresh", 35).clamp(0, 100) as i32;
                if engine.mic_loudness() >= threshold {
                    let hold_seconds = config.int("light_hold_s", 30).clamp(1, 3600) as u64;
                    sound_hold_until = Some(Instant::now() + Duration::from_secs(hold_seconds));
                }
                sound_hold_until.is_some_and(|until| Instant::now() < until)
            };
            if let Err(error) = gpio.write(on) {
                eprintln!("light controller: GPIO write failed: {error}");
            }
        }
        wait_or_stop(&stop, TICK);
    }
}

fn is_stopped(stop: &StopSignal) -> bool {
    *stop
        .stopped
        .lock()
        .unwrap_or_else(|error| error.into_inner())
}

fn wait_or_stop(stop: &StopSignal, duration: Duration) {
    let stopped = stop
        .stopped
        .lock()
        .unwrap_or_else(|error| error.into_inner());
    let _ = stop
        .wake
        .wait_timeout_while(stopped, duration, |stopped| !*stopped)
        .unwrap_or_else(|error| error.into_inner());
}

fn hour_in_window(hour: u32, start: u32, end: u32) -> bool {
    if start == end {
        true
    } else if start < end {
        hour >= start && hour < end
    } else {
        hour >= start || hour < end
    }
}

struct Gpio {
    number: i32,
    active_low: bool,
    logical_level: Option<bool>,
    value: File,
}

impl Gpio {
    fn open(number: i32, active_low: bool) -> Result<Self> {
        let _ = write_sysfs("/sys/class/gpio/export", &number.to_string());
        let directory = PathBuf::from(format!("/sys/class/gpio/gpio{number}"));
        write_sysfs(directory.join("direction"), "low")
            .with_context(|| format!("set gpio{number} direction"))?;
        let value = OpenOptions::new()
            .write(true)
            .open(directory.join("value"))
            .with_context(|| format!("open gpio{number} value"))?;
        Ok(Self {
            number,
            active_low,
            logical_level: None,
            value,
        })
    }

    fn set_active_low(&mut self, active_low: bool) {
        if self.active_low != active_low {
            self.active_low = active_low;
            self.logical_level = None;
        }
    }

    fn write(&mut self, on: bool) -> std::io::Result<()> {
        if self.logical_level == Some(on) {
            return Ok(());
        }
        let physical_high = on != self.active_low;
        self.value
            .write_all(if physical_high { b"1" } else { b"0" })?;
        self.value.flush()?;
        self.logical_level = Some(on);
        Ok(())
    }
}

impl Drop for Gpio {
    fn drop(&mut self) {
        let _ = self.write(false);
        let _ = write_sysfs("/sys/class/gpio/unexport", &self.number.to_string());
    }
}

fn write_sysfs(path: impl Into<PathBuf>, value: &str) -> std::io::Result<()> {
    OpenOptions::new()
        .write(true)
        .open(path.into())?
        .write_all(value.as_bytes())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn time_window_supports_daytime_overnight_and_full_day() {
        assert!(hour_in_window(12, 8, 18));
        assert!(!hour_in_window(18, 8, 18));
        assert!(hour_in_window(23, 22, 6));
        assert!(hour_in_window(5, 22, 6));
        assert!(!hour_in_window(12, 22, 6));
        assert!(hour_in_window(12, 0, 0));
    }
}
