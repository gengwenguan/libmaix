//! Camera Rust - Video surveillance system in Rust for M2dock board
//! 
//! This is a Rust implementation of the camera example from libmaix,
//! providing video capture, network streaming, and file recording.

#![allow(dead_code)]

pub mod camera;
pub mod encoder;
pub mod server;
pub mod terminal;
pub mod vo;
pub mod memory;
pub mod nal;
pub mod ffi;

pub use camera::Camera;
pub use server::file::FileManager;
pub use server::tcp::TcpServer;
pub use terminal::Terminal;
pub use vo::VideoOutput;

// 重新导出bindings，方便其他模块使用
pub use ffi::*;
