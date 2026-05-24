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
#   examples/camera/web_player.html  →  dist/web/index.html
#   将来要拆 app.js / style.css，往 examples/camera/web/ 下放即可
#   （如果以后 web/ 目录存在，会整目录复制；目前只有单文件，先按文件复制）
# 设备端运行时由 httpServer 通过 /proc/self/exe 解析自身目录、
# 读取 <exe_dir>/web/index.html。
# ----------------------------------------------------------------------
set(_web_src_html "${CMAKE_CURRENT_LIST_DIR}/web_player.html")
set(_web_dst_dir  "${PROJECT_DIST_DIR}/web")
set(_web_dst_html "${_web_dst_dir}/index.html")
if(EXISTS "${_web_src_html}")
    # 旧实现用 add_custom_command(TARGET ... POST_BUILD)，
    # 只在 camera 目标真正链接时才触发。当只改 web_player.html 而 C++ 源没变时，
    # camera 不会被重新链接，POST_BUILD 不跑，dist/web/index.html 永远是旧的。
    # 改为独立 custom_command + custom_target，让它依赖源 html，
    # 源文件 mtime 一变 → dist 文件被重生成，且 ALL 模式确保每次 build 都检查。
    add_custom_command(
        OUTPUT  "${_web_dst_html}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_web_dst_dir}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_web_src_html}" "${_web_dst_html}"
        DEPENDS "${_web_src_html}"
        COMMENT "-- copy web assets to ${_web_dst_html}"
    )
    add_custom_target(copy_web_assets ALL DEPENDS "${_web_dst_html}")
    add_dependencies(${PROJECT_NAME} copy_web_assets)
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


