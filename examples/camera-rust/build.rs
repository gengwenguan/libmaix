use std::env;
use std::path::{Path, PathBuf};

fn emit_link_search(path: impl AsRef<Path>) {
    println!("cargo:rustc-link-search=native={}", path.as_ref().display());
}

fn main() {
    println!("cargo:rerun-if-env-changed=LIBMAIX_SDK_PATH");
    println!("cargo:rerun-if-env-changed=CAMERA_CPP_DIR");
    println!("cargo:rerun-if-env-changed=CAMERA_BUILD_DIR");
    println!("cargo:rerun-if-env-changed=V831_TOOLCHAIN_PATH");
    println!("cargo:rerun-if-changed=native/camera_native.h");
    println!("cargo:rerun-if-changed=native/camera_native.cpp");
    println!("cargo:rerun-if-changed=native/camera_native_stub.cpp");

    let target = env::var("TARGET").expect("TARGET is set by Cargo");
    if target != "armv7-unknown-linux-musleabihf" {
        cc::Build::new()
            .cpp(true)
            .std("c++14")
            .file("native/camera_native_stub.cpp")
            .compile("camera_native");
        return;
    }

    let manifest = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());
    let sdk = env::var_os("LIBMAIX_SDK_PATH")
        .map(PathBuf::from)
        .unwrap_or_else(|| manifest.join("../.."));
    let camera = env::var_os("CAMERA_CPP_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(|| manifest.join("../camera"));
    let camera_build = env::var_os("CAMERA_BUILD_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(|| camera.join("build"));

    let src = camera.join("main/src");
    println!("cargo:rerun-if-changed={}", src.display());
    let mut build = cc::Build::new();
    build
        .cpp(true)
        .std("c++14")
        .warnings(true)
        .define("CAMERA_RUST_HOST", "1")
        .include("native")
        .include(camera.join("main/dep/codec/inc"))
        .include(camera.join("main/dep/asound/inc"))
        .include(camera.join("main/dep/ffmpeg/inc"))
        .include(camera.join("main/dep/openssl/inc"))
        .include(sdk.join("components/libmaix/include"))
        .include(sdk.join("components/maix_cv_image/include"))
        .include(sdk.join("components/libmaix/lib/arch/v83x/include/opencv4"))
        .include(camera_build.join("config"))
        .include(src.join("h264Enc"))
        .include(src.join("aacEnc"))
        .include(src.join("fmp4Muxer"))
        .include(src.join("personDetector"))
        .include(src.join("talkPlayer"))
        .include(src.join("tlsContext"))
        .include(src.join("utilTools"))
        .file("native/camera_native.cpp")
        .file(src.join("h264Enc/h264Enc.cpp"))
        .file(src.join("aacEnc/aacEnc.cpp"))
        .file(src.join("fmp4Muxer/fmp4Muxer.cpp"))
        .file(src.join("personDetector/personDetector.cpp"))
        .file(src.join("talkPlayer/talkPlayer.cpp"))
        .file(src.join("tlsContext/tlsContext.cpp"))
        .file(src.join("utilTools/logAdapt.cpp"));

    if let Some(toolchain) = env::var_os("V831_TOOLCHAIN_PATH") {
        build.compiler(PathBuf::from(toolchain).join("arm-openwrt-linux-muslgnueabi-g++"));
    }
    build.compile("camera_native");

    emit_link_search(camera_build.join("libmaix"));
    emit_link_search(sdk.join("components/libmaix/lib/arch/v831"));
    emit_link_search(sdk.join("components/libmaix/lib/arch/v83x/lib"));
    emit_link_search(sdk.join("components/libmaix/lib/arch/v83x/opencv4"));
    emit_link_search(camera.join("main/dep/ffmpeg/lib"));
    emit_link_search(camera.join("main/dep/asound/lib"));

    for lib in [
        "maix_nn",
        "jpeg",
        "png12",
        "webp",
        "freetype",
        "harfbuzz",
        "glib-2.0",
        "pcre",
        "bz2",
        "opencv_imgcodecs",
        "opencv_imgproc",
        "opencv_core",
        "z",
        "ISP",
        "cdc_base",
        "ion",
        "glog",
        "log",
        "VE",
        "isp_ini",
        "media_utils",
        "MemAdapter",
        "mpp_isp",
        "mpp_vi",
        "maix_cam",
        "maix_disp",
        "maix_image",
        "maix_utils",
        "cdx_base",
        "videoengine",
        "cdx_stream",
        "cdx_parser",
        "vdecoder",
        "venc_codec",
        "venc_base",
        "mpp_vo",
        "media_mpp",
        "cedarxrender",
        "cedarxstream",
        "cdx_common",
        "adecoder",
        "hwdisplay",
        "cutils",
        "cedarx_aencoder",
        "asound",
        "mpp_component",
        "vencoder",
        "opus",
        "ssl",
        "crypto",
        "avutil",
        "avcodec",
        "x264",
        "avdevice",
        "avformat",
        "swresample",
        "swscale",
        "stdc++",
        "m",
        "pthread",
        "dl",
        "gcc_s",
        "c",
    ] {
        println!("cargo:rustc-link-lib=dylib={lib}");
    }
    println!(
        "cargo:rustc-link-arg={}",
        camera_build.join("libmaix/liblibmaix.a").display()
    );
    println!(
        "cargo:rustc-link-arg=-Wl,-rpath-link,{}",
        camera.join("main/dep/ffmpeg/lib").display()
    );
}
