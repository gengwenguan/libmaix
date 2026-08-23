use crate::native::{TlsConnection, TlsContext};
use anyhow::{Context, Result};
use std::io::{ErrorKind, Read, Write};
use std::net::{Shutdown, TcpStream};
use std::os::fd::AsRawFd;
use std::sync::Arc;
use std::thread;
use std::time::{Duration, Instant};

pub struct Connection {
    stream: TcpStream,
    tls: Option<TlsConnection>,
}

impl Connection {
    pub fn new(stream: TcpStream, tls: Option<Arc<TlsContext>>) -> Result<Self> {
        stream.set_nonblocking(true)?;
        stream.set_nodelay(true)?;
        let tls = tls
            .map(|context| context.accept(stream.as_raw_fd()))
            .transpose()
            .context("TLS accept")?;
        Ok(Self { stream, tls })
    }

    pub fn read_some(&mut self, output: &mut [u8]) -> std::io::Result<Option<usize>> {
        if let Some(tls) = &mut self.tls {
            tls.read(output)
        } else {
            match self.stream.read(output) {
                Ok(size) => Ok(Some(size)),
                Err(error)
                    if matches!(error.kind(), ErrorKind::WouldBlock | ErrorKind::Interrupted) =>
                {
                    Ok(None)
                }
                Err(error) => Err(error),
            }
        }
    }

    pub fn write_all_timeout(&mut self, input: &[u8], timeout: Duration) -> Result<()> {
        let deadline = Instant::now() + timeout;
        let mut offset = 0;
        while offset < input.len() {
            if Instant::now() >= deadline {
                anyhow::bail!("connection write timeout");
            }
            let written = if let Some(tls) = &mut self.tls {
                tls.write(&input[offset..])?
            } else {
                match self.stream.write(&input[offset..]) {
                    Ok(size) => Some(size),
                    Err(error)
                        if matches!(
                            error.kind(),
                            ErrorKind::WouldBlock | ErrorKind::Interrupted
                        ) =>
                    {
                        None
                    }
                    Err(error) => return Err(error.into()),
                }
            };
            match written {
                Some(0) => anyhow::bail!("connection closed during write"),
                Some(size) => offset += size,
                None => thread::sleep(Duration::from_millis(2)),
            }
        }
        Ok(())
    }

    pub fn read_until(
        &mut self,
        delimiter: &[u8],
        maximum: usize,
        timeout: Duration,
    ) -> Result<Vec<u8>> {
        let deadline = Instant::now() + timeout;
        let mut data = Vec::with_capacity(maximum.min(4096));
        let mut buffer = [0u8; 4096];
        loop {
            if data
                .windows(delimiter.len())
                .any(|window| window == delimiter)
            {
                return Ok(data);
            }
            if data.len() >= maximum {
                anyhow::bail!("request exceeds {maximum} bytes");
            }
            if Instant::now() >= deadline {
                anyhow::bail!("connection read timeout");
            }
            match self.read_some(&mut buffer)? {
                Some(0) => anyhow::bail!("connection closed"),
                Some(size) => {
                    let remaining = maximum - data.len();
                    data.extend_from_slice(&buffer[..size.min(remaining)]);
                }
                None => thread::sleep(Duration::from_millis(2)),
            }
        }
    }

    pub fn peer(&self) -> String {
        self.stream
            .peer_addr()
            .map(|address| address.to_string())
            .unwrap_or_else(|_| "unknown".to_owned())
    }

    pub fn shutdown(&self) {
        let _ = self.stream.shutdown(Shutdown::Both);
    }
}
