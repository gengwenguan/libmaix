use crate::config::RuntimeConfig;
use crate::netinfo;
use anyhow::{Context, Result};
use std::io::{Read, Write};
use std::net::{TcpStream, ToSocketAddrs};
use std::sync::{Arc, Condvar, Mutex};
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant};

const NETWORK_TIMEOUT: Duration = Duration::from_secs(5);

struct StopSignal {
    stopped: Mutex<bool>,
    wake: Condvar,
}

pub struct MqttReporter {
    stop: Arc<StopSignal>,
    thread: Option<JoinHandle<()>>,
}

impl MqttReporter {
    pub fn start(config: Arc<RuntimeConfig>) -> Result<Self> {
        let stop = Arc::new(StopSignal {
            stopped: Mutex::new(false),
            wake: Condvar::new(),
        });
        let thread_stop = stop.clone();
        let thread = thread::Builder::new()
            .name("mqtt-reporter".to_owned())
            .stack_size(160 * 1024)
            .spawn(move || reporter_loop(config, thread_stop))
            .context("spawn MQTT reporter")?;
        Ok(Self {
            stop,
            thread: Some(thread),
        })
    }
}

impl Drop for MqttReporter {
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

fn reporter_loop(config: Arc<RuntimeConfig>, stop: Arc<StopSignal>) {
    let mut last_address = String::new();
    let mut last_publish = None::<Instant>;
    loop {
        if is_stopped(&stop) {
            return;
        }
        let enabled = config.bool("mqtt_enabled", false);
        let poll_seconds = config.int("mqtt_poll_sec", 10).clamp(2, 3600) as u64;
        if enabled {
            let interface = config.string("mqtt_iface", "wlan0");
            let address = netinfo::global_ipv6(&interface);
            let report_seconds = config.int("mqtt_report_interval_s", 3600).clamp(0, 86400) as u64;
            let keepalive_due = report_seconds > 0
                && last_publish
                    .is_some_and(|last| last.elapsed() >= Duration::from_secs(report_seconds));
            if !address.is_empty() && (address != last_address || keepalive_due) {
                let broker = config.string("mqtt_broker_host", "broker.emqx.io");
                let port = config.int("mqtt_broker_port", 1883).clamp(1, 65535) as u16;
                let topic = config.string("mqtt_topic", "cam/ipv6");
                let client_id = config.string("mqtt_client_id", "v831cam");
                let retain = config.bool("mqtt_retain", true);
                match publish(
                    &broker,
                    port,
                    &client_id,
                    &topic,
                    address.as_bytes(),
                    retain,
                ) {
                    Ok(()) => {
                        eprintln!("MQTT published {topic}={address}");
                        last_address = address;
                        last_publish = Some(Instant::now());
                    }
                    Err(error) => eprintln!("MQTT publish failed: {error:#}"),
                }
            }
        }
        wait_or_stop(&stop, Duration::from_secs(poll_seconds));
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

fn publish(
    broker: &str,
    port: u16,
    client_id: &str,
    topic: &str,
    payload: &[u8],
    retain: bool,
) -> Result<()> {
    let addresses = (broker, port)
        .to_socket_addrs()
        .with_context(|| format!("resolve MQTT broker {broker}:{port}"))?;
    let mut stream = addresses
        .filter_map(|address| TcpStream::connect_timeout(&address, NETWORK_TIMEOUT).ok())
        .next()
        .with_context(|| format!("connect MQTT broker {broker}:{port}"))?;
    stream.set_read_timeout(Some(NETWORK_TIMEOUT))?;
    stream.set_write_timeout(Some(NETWORK_TIMEOUT))?;

    stream.write_all(&connect_packet(client_id)?)?;
    let mut connack = [0u8; 4];
    stream.read_exact(&mut connack)?;
    if connack != [0x20, 0x02, 0x00, 0x00] {
        anyhow::bail!("MQTT CONNACK rejected: {connack:02x?}");
    }
    stream.write_all(&publish_packet(topic, payload, retain)?)?;
    stream.write_all(&[0xe0, 0x00])?;
    Ok(())
}

fn connect_packet(client_id: &str) -> Result<Vec<u8>> {
    let mut body = Vec::new();
    push_utf8(&mut body, "MQTT")?;
    body.push(4); // MQTT 3.1.1
    body.push(0x02); // clean session
    body.extend_from_slice(&30u16.to_be_bytes());
    push_utf8(&mut body, client_id)?;

    let mut packet = vec![0x10];
    push_remaining_length(&mut packet, body.len())?;
    packet.extend_from_slice(&body);
    Ok(packet)
}

fn publish_packet(topic: &str, payload: &[u8], retain: bool) -> Result<Vec<u8>> {
    let mut body = Vec::with_capacity(topic.len() + payload.len() + 2);
    push_utf8(&mut body, topic)?;
    body.extend_from_slice(payload);

    let mut packet = vec![0x30 | u8::from(retain)];
    push_remaining_length(&mut packet, body.len())?;
    packet.extend_from_slice(&body);
    Ok(packet)
}

fn push_utf8(output: &mut Vec<u8>, value: &str) -> Result<()> {
    let length = u16::try_from(value.len()).context("MQTT string is too long")?;
    output.extend_from_slice(&length.to_be_bytes());
    output.extend_from_slice(value.as_bytes());
    Ok(())
}

fn push_remaining_length(output: &mut Vec<u8>, mut length: usize) -> Result<()> {
    if length > 268_435_455 {
        anyhow::bail!("MQTT packet is too large");
    }
    loop {
        let mut byte = (length % 128) as u8;
        length /= 128;
        if length > 0 {
            byte |= 0x80;
        }
        output.push(byte);
        if length == 0 {
            return Ok(());
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn encodes_connect_packet() {
        let packet = connect_packet("cam").unwrap();
        assert_eq!(packet[0], 0x10);
        assert_eq!(&packet[2..8], &[0, 4, b'M', b'Q', b'T', b'T']);
        assert!(packet.ends_with(&[0, 3, b'c', b'a', b'm']));
    }

    #[test]
    fn encodes_multibyte_remaining_length() {
        let packet = publish_packet("topic", &[0u8; 200], true).unwrap();
        assert_eq!(packet[0], 0x31);
        assert_ne!(packet[1] & 0x80, 0);
    }
}
