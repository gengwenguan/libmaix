cmake_minimum_required(VERSION 3.5)


# Get SDK path
if(NOT SDK_PATH)
    get_filename_component(SDK_PATH ../../ ABSOLUTE)
    if(EXISTS $ENV{LIBMAIX_SDK_PATH})
        set(SDK_PATH $ENV{LIBMAIX_SDK_PATH})
    endif()
endif()

# Check SDK Path
if(NOT EXISTS ${SDK_PATH})
    message(FATAL_ERROR "SDK path Error, Please set SDK_PATH or LIBMAIX_SDK_PATH variable")
endif()

# Get Toolchain path
if(NOT CONFIG_TOOLCHAIN_PATH)
    if(EXISTS $ENV{LIBMAIX_TOOLCHAIN_PATH})
        set(CONFIG_TOOLCHAIN_PATH $ENV{LIBMAIX_TOOLCHAIN_PATH})
    endif()
endif()

## Add preprocessor definitions for whole project
# add_definitions(-DAAAAA=1)

# Call compile
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
include(${SDK_PATH}/tools/cmake/compile.cmake)


# Project Name, default the same as project directory name
get_filename_component(parent_dir ${CMAKE_PARENT_LIST_FILE} DIRECTORY)
get_filename_component(project_dir_name ${parent_dir} NAME)

set(PROJECT_NAME ${project_dir_name}) # change this var if don't want the same as directory's

message("-- PROJECT_NAME:${PROJECT_NAME}")
project(${PROJECT_NAME})

# ----------------------------------------------------------------------
# 拷贝前端资源到 dist/web/
# 约定：
#   examples/camera/web/  →  dist/web/ （整目录复制）
# 新增 web 资源（style.css、app.js 等）直接放到 web/ 目录下即可，
# CMake 会自动检测并复制。
# 设备端运行时由 httpServer 通过 /proc/self/exe 解析自身目录、
# 读取 <exe_dir>/web/index.html。
# ----------------------------------------------------------------------
set(_web_src_dir "${CMAKE_CURRENT_LIST_DIR}/web")
set(_web_dst_dir "${PROJECT_DIST_DIR}/web")
if(EXISTS "${_web_src_dir}")
    # 每次构建都整目录镜像 web/ → dist/web/。
    # 不用 file(GLOB) 在配置期枚举文件：GLOB 是配置期快照，新增文件（如
    # favicon.svg）若未重跑 cmake 配置就会漏拷，导致板上缺文件。copy_directory
    # 与 cmake 版本无关、递归复制，保证任何新增/修改的 web 资源都会被带上。
    add_custom_target(copy_web_assets ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_web_dst_dir}"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${_web_src_dir}" "${_web_dst_dir}"
        COMMENT "-- mirror web/ -> dist/web/"
    )
    add_dependencies(${PROJECT_NAME} copy_web_assets)
endif()

# ----------------------------------------------------------------------
# 拷贝提示音资源到 dist/prompt/
# 约定：
#   examples/camera/door.wav     →  dist/prompt/door.wav
#   examples/camera/*.wav        →  dist/prompt/*.wav    （未来新增）
# 设备端运行时由 C_TalkPlayer::PlayWavSync 通过 <exe_dir>/prompt/<name>.wav 加载。
# 第一版只支持 16-bit / 48kHz / mono PCM（与 ALSA 输出参数完全一致），
# 不需要重采样；新加 wav 文件请先用 ffmpeg/sox 转成同样参数。
# ----------------------------------------------------------------------
file(GLOB _prompt_src_wavs "${CMAKE_CURRENT_LIST_DIR}/*.wav")
if(_prompt_src_wavs)
    set(_prompt_dst_dir "${PROJECT_DIST_DIR}/prompt")
    set(_prompt_dst_wavs)
    foreach(_src_wav ${_prompt_src_wavs})
        get_filename_component(_wav_name "${_src_wav}" NAME)
        set(_dst_wav "${_prompt_dst_dir}/${_wav_name}")
        list(APPEND _prompt_dst_wavs "${_dst_wav}")
        add_custom_command(
            OUTPUT  "${_dst_wav}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${_prompt_dst_dir}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_src_wav}" "${_dst_wav}"
            DEPENDS "${_src_wav}"
            COMMENT "-- copy prompt wav to ${_dst_wav}"
        )
    endforeach()
    add_custom_target(copy_prompt_assets ALL DEPENDS ${_prompt_dst_wavs})
    add_dependencies(${PROJECT_NAME} copy_prompt_assets)
endif()

# ----------------------------------------------------------------------
# 拷贝内存看门狗脚本到 dist/
#   examples/camera/mem_watchdog.sh  →  dist/mem_watchdog.sh
# 设备端常驻运行：监控 camera VmData，超 80MB 自动重启（应对闭源库慢泄漏）。
# 开机自启需手动挂到 /etc/init.d/S02app（见 mem_watchdog.sh 顶部注释）。
# ----------------------------------------------------------------------
set(_wd_src "${CMAKE_CURRENT_LIST_DIR}/mem_watchdog.sh")
set(_wd_dst "${PROJECT_DIST_DIR}/mem_watchdog.sh")
if(EXISTS "${_wd_src}")
    add_custom_command(
        OUTPUT  "${_wd_dst}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_wd_src}" "${_wd_dst}"
        DEPENDS "${_wd_src}"
        COMMENT "-- copy mem_watchdog.sh to ${_wd_dst}"
    )
    add_custom_target(copy_watchdog ALL DEPENDS "${_wd_dst}")
    add_dependencies(${PROJECT_NAME} copy_watchdog)
endif()


# ----------------------------------------------------------------------
# 拷贝 TLS 自签证书到 dist/cert/
# 设备端运行时通过 <exe_dir>/cert/server.crt + server.key 加载。
# 证书生成方式（Mac/编译机均可）：
#   cd examples/camera/cert && \
#   openssl req -x509 -newkey rsa:2048 -nodes \
#     -keyout server.key -out server.crt -days 3650 \
#     -subj "/CN=192.168.1.28" \
#     -addext "subjectAltName=IP:192.168.1.28,IP:127.0.0.1,DNS:localhost"
# 证书是数据，与 OpenSSL 链接库版本无关；用本机 openssl 3.x 生成 1.1.x 也能用
# ----------------------------------------------------------------------
set(_cert_src_dir "${CMAKE_CURRENT_LIST_DIR}/cert")
set(_cert_dst_dir "${PROJECT_DIST_DIR}/cert")
set(_cert_dst_crt "${_cert_dst_dir}/server.crt")
set(_cert_dst_key "${_cert_dst_dir}/server.key")
if(EXISTS "${_cert_src_dir}/server.crt" AND EXISTS "${_cert_src_dir}/server.key")
    # 同样的 POST_BUILD 失效问题：换证书但 C++ 源没变会导致 dist/cert 不更新。
    add_custom_command(
        OUTPUT  "${_cert_dst_crt}" "${_cert_dst_key}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_cert_dst_dir}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_cert_src_dir}/server.crt" "${_cert_dst_crt}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_cert_src_dir}/server.key" "${_cert_dst_key}"
        DEPENDS "${_cert_src_dir}/server.crt" "${_cert_src_dir}/server.key"
        COMMENT "-- copy TLS cert to ${_cert_dst_dir}/"
    )
    add_custom_target(copy_tls_cert ALL DEPENDS "${_cert_dst_crt}" "${_cert_dst_key}")
    add_dependencies(${PROJECT_NAME} copy_tls_cert)
else()
    message(WARNING "TLS cert not found at ${_cert_src_dir}; HTTPS will fail at runtime")
endif()

