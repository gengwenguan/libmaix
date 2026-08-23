use serde_json::{json, Value};
use std::ffi::CString;
use std::fs;
use std::path::Path;
use std::sync::Mutex;

#[derive(Default)]
pub struct SysInfo {
    last_cpu: Mutex<Option<(u64, u64)>>,
}

impl SysInfo {
    pub fn sample(&self, disk_path: &Path) -> Value {
        let (cpu_valid, cpu_percent, cores) = self.cpu();
        let load = read_numbers("/proc/loadavg");
        let memory = read_key_values("/proc/meminfo");
        let process = read_key_values("/proc/self/status");
        let uptime = read_numbers("/proc/uptime").first().copied().unwrap_or(0.0);
        let process_uptime = process_uptime(uptime);
        let (disk_valid, disk_total, disk_available) = disk_usage(disk_path);

        json!({
            "ok": true,
            "cpu": {
                "valid": cpu_valid,
                "percent": cpu_percent,
                "cores": cores
            },
            "load": {
                "valid": load.len() >= 3,
                "l1": load.first().copied().unwrap_or(0.0),
                "l5": load.get(1).copied().unwrap_or(0.0),
                "l15": load.get(2).copied().unwrap_or(0.0)
            },
            "mem": {
                "valid": memory.contains_key("MemTotal"),
                "total_kb": memory.get("MemTotal").copied().unwrap_or(0),
                "avail_kb": memory.get("MemAvailable")
                    .or_else(|| memory.get("MemFree")).copied().unwrap_or(0)
            },
            "proc": {
                "valid": process.contains_key("VmData"),
                "vmrss_kb": process.get("VmRSS").copied().unwrap_or(0),
                "vmdata_kb": process.get("VmData").copied().unwrap_or(0),
                "threshold_kb": 40960
            },
            "disk": {
                "valid": disk_valid,
                "total_bytes": disk_total,
                "avail_bytes": disk_available
            },
            "uptime": {
                "valid": uptime > 0.0,
                "sec": uptime as u64
            },
            "proc_uptime": {
                "valid": process_uptime.is_some(),
                "sec": process_uptime.unwrap_or(0)
            }
        })
    }

    fn cpu(&self) -> (bool, f64, usize) {
        let Ok(stat) = fs::read_to_string("/proc/stat") else {
            return (false, 0.0, 0);
        };
        let mut aggregate = None;
        let mut cores = 0;
        for line in stat.lines() {
            if let Some(rest) = line.strip_prefix("cpu ") {
                let values = rest
                    .split_whitespace()
                    .filter_map(|value| value.parse::<u64>().ok())
                    .collect::<Vec<_>>();
                if values.len() >= 4 {
                    let idle = values[3] + values.get(4).copied().unwrap_or(0);
                    aggregate = Some((values.iter().sum::<u64>(), idle));
                }
            } else if line
                .strip_prefix("cpu")
                .and_then(|value| value.split_whitespace().next())
                .is_some_and(|value| value.chars().all(|ch| ch.is_ascii_digit()))
            {
                cores += 1;
            }
        }
        let Some(current) = aggregate else {
            return (false, 0.0, cores);
        };
        let mut previous = self
            .last_cpu
            .lock()
            .unwrap_or_else(|error| error.into_inner());
        let result = previous.map(|old| {
            let total = current.0.saturating_sub(old.0);
            let idle = current.1.saturating_sub(old.1);
            if total == 0 {
                0.0
            } else {
                (total.saturating_sub(idle)) as f64 * 100.0 / total as f64
            }
        });
        *previous = Some(current);
        (result.is_some(), result.unwrap_or(0.0), cores)
    }
}

fn process_uptime(system_uptime: f64) -> Option<u64> {
    let stat = fs::read_to_string("/proc/self/stat").ok()?;
    // comm (field 2) may contain spaces or parentheses. Fields after the last
    // ')' begin at field 3; starttime is field 22, therefore index 19 here.
    let mut fields = stat.get(stat.rfind(')')? + 1..)?.split_whitespace();
    let start_ticks = fields.nth(19)?.parse::<u64>().ok()?;
    let ticks_per_second = unsafe { libc::sysconf(libc::_SC_CLK_TCK) };
    if ticks_per_second <= 0 {
        return None;
    }
    Some((system_uptime.max(0.0) as u64).saturating_sub(start_ticks / ticks_per_second as u64))
}

fn read_numbers(path: &str) -> Vec<f64> {
    fs::read_to_string(path)
        .unwrap_or_default()
        .split_whitespace()
        .filter_map(|value| value.parse().ok())
        .collect()
}

fn read_key_values(path: &str) -> std::collections::BTreeMap<String, u64> {
    fs::read_to_string(path)
        .unwrap_or_default()
        .lines()
        .filter_map(|line| {
            let (key, value) = line.split_once(':')?;
            let value = value.split_whitespace().next()?.parse().ok()?;
            Some((key.to_owned(), value))
        })
        .collect()
}

fn disk_usage(path: &Path) -> (bool, u64, u64) {
    let Ok(path) = CString::new(path.to_string_lossy().as_bytes()) else {
        return (false, 0, 0);
    };
    let mut stat = std::mem::MaybeUninit::<libc::statvfs>::uninit();
    let result = unsafe { libc::statvfs(path.as_ptr(), stat.as_mut_ptr()) };
    if result != 0 {
        return (false, 0, 0);
    }
    let stat = unsafe { stat.assume_init() };
    (
        true,
        integer_to_u64(stat.f_blocks).saturating_mul(integer_to_u64(stat.f_frsize)),
        integer_to_u64(stat.f_bavail).saturating_mul(integer_to_u64(stat.f_frsize)),
    )
}

fn integer_to_u64<T: TryInto<u64>>(value: T) -> u64 {
    value.try_into().ok().unwrap_or(u64::MAX)
}
