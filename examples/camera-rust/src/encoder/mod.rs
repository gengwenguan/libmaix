use anyhow::Result;

pub trait Encoder {
    fn encode(&mut self, data: &[u8]) -> Result<Vec<u8>>;
    fn flush(&mut self) -> Result<Vec<u8>>;
    fn stop(&mut self) -> Result<()>;
}

pub mod h264;
pub mod opus;