use std::env;
use std::path::PathBuf;

fn main() {
    // 设置链接库（使用动态库）
    println!("cargo:rustc-link-lib=dylib=maix_cam");
    println!("cargo:rustc-link-lib=dylib=maix_disp");
    println!("cargo:rustc-link-lib=dylib=maix_image");
    println!("cargo:rustc-link-lib=dylib=maix_utils");
    println!("cargo:rustc-link-lib=dylib=maix_nn");
    println!("cargo:rustc-link-lib=dylib=ISP");
    println!("cargo:rustc-link-lib=dylib=VE");
    println!("cargo:rustc-link-lib=dylib=ion");
    println!("cargo:rustc-link-lib=dylib=mpp_vi");
    println!("cargo:rustc-link-lib=dylib=mpp_vo");
    println!("cargo:rustc-link-lib=dylib=media_mpp");
    println!("cargo:rustc-link-lib=dylib=vencoder");
    println!("cargo:rustc-link-lib=dylib=venc_codec");
    println!("cargo:rustc-link-lib=dylib=videoengine");
    
    // ALSA库
    println!("cargo:rustc-link-lib=dylib=asound");
    
    // FFmpeg库
    println!("cargo:rustc-link-lib=dylib=avcodec");
    println!("cargo:rustc-link-lib=dylib=avformat");
    println!("cargo:rustc-link-lib=dylib=avutil");
    println!("cargo:rustc-link-lib=dylib=avdevice");
    
    // 设置库搜索路径
    println!("cargo:rustc-link-search=/root/work/libmaix/examples/camera/dist/lib");
    
    // 生成绑定
    let bindings = bindgen::Builder::default()
        .header("wrapper.h")
        .clang_arg("-I/root/work/libmaix/components/libmaix/include")
        .clang_arg("-I/root/work/libmaix/examples/camera/main")
        .clang_arg("-I/root/work/libmaix/examples/camera/main/dep/codec/inc")
        .clang_arg("-I/root/work/libmaix/examples/camera/main/dep/asound/inc")
        .clang_arg("-I/root/work/libmaix/examples/camera/main/dep/ffmpeg/inc")
        .clang_arg("-I/opt/toolchain-sunxi-musl/toolchain/arm-openwrt-linux-muslgnueabi/include")
        // 排除重复的浮点常量定义
        .blocklist_item("FP_NAN")
        .blocklist_item("FP_INFINITE")
        .blocklist_item("FP_ZERO")
        .blocklist_item("FP_SUBNORMAL")
        .blocklist_item("FP_NORMAL")
        .generate()
        .expect("Unable to generate bindings");
    
    let out_path = PathBuf::from(env::var("OUT_DIR").unwrap());
    bindings
        .write_to_file(out_path.join("bindings.rs"))
        .expect("Could not write bindings");
}
