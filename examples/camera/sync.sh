#!/usr/bin/env bash
# ============================================================
# sync.sh —— 本地 → 编译机(192.168.1.10) → 开发板(192.168.1.28)
#
# 用法：
#   ./sync.sh           # 仅同步源码到编译机
#   ./sync.sh build     # 同步 + 在编译机上 python3 project.py build
#   ./sync.sh push      # 同步 + 编译 + scp 产物到开发板 + 重启 camera
#   ./sync.sh run       # 仅推产物到开发板并启动（不重新编译）
#   ./sync.sh clean     # 清理编译机上的 build/dist
#   ./sync.sh log       # tail 开发板上的 camera 运行日志
# ============================================================
set -e

# ---------- 配置 ----------
BUILD_USER="root"
BUILD_HOST="2409:8a1e:7a45:8ad0:8647:9ff:fe45:35a0"
BUILD_DIR="/root/work/libmaix/examples/camera"

DEVICE_USER="root"
DEVICE_HOST="192.168.1.13"
DEVICE_DIR="/root/maix_dist"
DEVICE_PASS="root"          # 开发板 ssh 密码（与 scppush.sh 保持一致）

# 启动 camera 时注入的环境变量。每行一条 KEY=VALUE，留空即不注入。
# 调试用法：
#   EXTRA_ENV="AAC_DUMP_PATH=/tmp/test.aac" ./sync.sh push
#   EXTRA_ENV=$'MIC_PCM_DUMP_PATH=/tmp/mic.wav\nMIC_PCM_DUMP_SEC=30' ./sync.sh push
# 验证完后置空，避免每次都 dump 占用空间。
# 录像参数（segment / retain / max_bytes）已统一从 web 配置面板读取，
# 不再支持 RECORD_* 环境变量，留空即可。
EXTRA_ENV="${EXTRA_ENV:-}"

# 兼容老版本 sshd 的算法白名单：开发板通常只提供 ssh-rsa(SHA-1)，
# 新版 OpenSSH(>=8.7) 默认禁用，必须显式打开。
SSH_COMPAT_OPTS="-o StrictHostKeyChecking=no \
                 -o UserKnownHostsFile=/dev/null \
                 -o HostKeyAlgorithms=+ssh-rsa \
                 -o PubkeyAcceptedAlgorithms=+ssh-rsa \
                 -o KexAlgorithms=+diffie-hellman-group1-sha1,diffie-hellman-group14-sha1"

LOCAL_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# IPv6 字面量（含 ':'）无法直接放进 rsync/scp 的 user@host:path 语法，也无法作为
# ssh 的 hostname 直接 resolve（macOS 自带 openrsync/ssh 都不认 [ipv6] 方括号写法，
# 会报 "Could not resolve hostname [2409..."）。
#
# 统一解法："不含冒号的占位别名 + ssh -o HostName=<真实地址> 注入"：
#   - rsync/ssh/scp 看到的 host 是别名 build6，里头没有冒号，不会被错拆；
#   - 真正要连的 IPv6 地址通过 ssh 的 -o HostName= 解析。
# 这套写法在 IPv4 / IPv6 / 域名下都成立（IPv4 时别名就是地址本身、注入项为空）。
if [[ "${BUILD_HOST}" == *:* ]]; then
    BUILD_SSH_TARGET="build6"                         # 不含冒号的占位别名
    BUILD_SSH_OPT="-o HostName=${BUILD_HOST}"          # ssh 据此连真实 IPv6
else
    BUILD_SSH_TARGET="${BUILD_HOST}"
    BUILD_SSH_OPT=""
fi

# ---------- 颜色 ----------
GREEN='\033[0;32m'; YELLOW='\033[1;33m'; RED='\033[0;31m'; NC='\033[0m'
log()  { echo -e "${GREEN}[sync]${NC} $*"; }
warn() { echo -e "${YELLOW}[warn]${NC} $*"; }
err()  { echo -e "${RED}[err]${NC} $*" >&2; }

# ---------- Mac → 编译机 同步 ----------
do_sync() {
    log "rsync ${LOCAL_DIR}/  →  ${BUILD_USER}@${BUILD_HOST}:${BUILD_DIR}/"
    rsync -avz --delete \
        --exclude='.git/' \
        --exclude='dist/' \
        --exclude='build/' \
        --exclude='.cache/' \
        --exclude='compile_commands.json' \
        --exclude='*.o' \
        --exclude='*.d' \
        --exclude='core' \
        --exclude='core.*' \
        --exclude='video/' \
        --exclude='.DS_Store' \
        -e "ssh -o StrictHostKeyChecking=no ${BUILD_SSH_OPT}" \
        "${LOCAL_DIR}/" "${BUILD_USER}@${BUILD_SSH_TARGET}:${BUILD_DIR}/"
    log "源码同步完成"
}

# ---------- 编译机：执行 build ----------
do_build() {
    log "在 ${BUILD_HOST} 执行 python3 project.py build"
    ssh -o StrictHostKeyChecking=no ${BUILD_SSH_OPT} "${BUILD_USER}@${BUILD_SSH_TARGET}" \
        "cd ${BUILD_DIR} && python3 project.py build"
    log "编译完成，产物：${BUILD_HOST}:${BUILD_DIR}/dist/camera"
}

# ---------- 编译机 → 开发板：推送产物并启动 ----------
do_push_device() {
    log "在 ${BUILD_HOST} 上推送 dist/camera → ${DEVICE_HOST}:${DEVICE_DIR}"

    # Mac 本地拼好 export 串，避免远端嵌套引号
    local EXPORT_LINES=""
    if [ -n "${EXTRA_ENV}" ]; then
        EXPORT_LINES=$(printf '%s\n' "${EXTRA_ENV}" | awk 'NF{printf "export %s; ", $0}')
        log "注入环境变量: ${EXTRA_ENV}"
    fi

    ssh -o StrictHostKeyChecking=no ${BUILD_SSH_OPT} "${BUILD_USER}@${BUILD_SSH_TARGET}" "
        set -e
        cd ${BUILD_DIR}

        echo '[sync] 1) 停止开发板上旧的 camera 进程（避免 Text file busy / 设备抢占）'
        # 注意：V831 的 cedar/VI/disp/snd 都是独占设备，旧进程被 SIGKILL 后内核需要
        # 一段时间才能真正回收 fd（异步释放 ION/cedar_dev/v4l2 buffer）。如果 sleep
        # 不够，新 camera 起来后会卡在 libmaix_camera_module_init / VeInitialize 里
        # 几十秒，最终被 OOM 杀掉，表现为推送后黑屏 + SSH 极慢。
        # 这里采用：pidof 轮询 + 额外 sleep 2s 给驱动收尾。
        sshpass -p '${DEVICE_PASS}' ssh \
            ${SSH_COMPAT_OPTS} \
            ${DEVICE_USER}@${DEVICE_HOST} \
            'killall -9 camera 2>/dev/null; killall -9 start_app.sh 2>/dev/null;
             # 等所有 camera 工作线程退出（最多 10 秒）
             for i in 1 2 3 4 5 6 7 8 9 10; do
                 pidof camera >/dev/null 2>&1 || break
                 sleep 1
             done;
             # 再额外 sleep 2s，等内核异步回收 cedar_dev / video0 / disp / snd 的 fd
             sleep 2;
             exit 0'

        echo '[sync] 2) scp 推送新二进制'
        sshpass -p '${DEVICE_PASS}' scp -r -O \
            ${SSH_COMPAT_OPTS} \
            dist/camera ${DEVICE_USER}@${DEVICE_HOST}:${DEVICE_DIR}

        echo '[sync] 2.1) scp 推送 web 静态资源 (dist/web → ${DEVICE_DIR}/web)'
        if [ -d dist/web ]; then
            # 先删板上旧 web/ 再整目录推：scp 只覆盖同名文件、不清理已删除项，
            # 先 rm 可保证板上与本地完全一致（既补新增如 favicon.svg，又清残留）。
            sshpass -p '${DEVICE_PASS}' ssh ${SSH_COMPAT_OPTS} \
                ${DEVICE_USER}@${DEVICE_HOST} 'rm -rf ${DEVICE_DIR}/web'
            sshpass -p '${DEVICE_PASS}' scp -r -O \
                ${SSH_COMPAT_OPTS} \
                dist/web ${DEVICE_USER}@${DEVICE_HOST}:${DEVICE_DIR}/
        else
            echo '[sync] !! dist/web 不存在，跳过 web 资源推送'
        fi

        echo '[sync] 2.2) scp 推送 TLS 自签证书 (dist/cert → ${DEVICE_DIR}/cert)'
        if [ -d dist/cert ]; then
            sshpass -p '${DEVICE_PASS}' scp -r -O \
                ${SSH_COMPAT_OPTS} \
                dist/cert ${DEVICE_USER}@${DEVICE_HOST}:${DEVICE_DIR}/
        else
            echo '[sync] !! dist/cert 不存在，跳过证书推送 (HTTPS 将启动失败)'
        fi

        echo '[sync] 2.3) scp 推送内存看门狗脚本 (dist/mem_watchdog.sh)'
        if [ -f dist/mem_watchdog.sh ]; then
            sshpass -p '${DEVICE_PASS}' scp -O \
                ${SSH_COMPAT_OPTS} \
                dist/mem_watchdog.sh ${DEVICE_USER}@${DEVICE_HOST}:${DEVICE_DIR}/
        else
            echo '[sync] !! dist/mem_watchdog.sh 不存在，跳过看门狗推送'
        fi

        echo '[sync] 3) 启动 camera (后台运行)'
        # 开发板 BusyBox 既无 nohup 也无 setsid。
        # 用子 shell + trap 屏蔽 HUP + 关闭所有继承自 ssh 的 fd 来后台启动。
        sshpass -p '${DEVICE_PASS}' ssh \
            ${SSH_COMPAT_OPTS} \
            ${DEVICE_USER}@${DEVICE_HOST} \
            'cd ${DEVICE_DIR} && rm -f camera.log && ( trap \"\" HUP; ${EXPORT_LINES} ./start_app.sh </dev/null >camera.log 2>&1 ) & sleep 1; exit 0'
    "
    log "推送完成，开发板已启动 camera"
}

# ---------- 从开发板拉文件回 Mac ----------
# 用法：./sync.sh pull /tmp/test.aac           ← 拉到当前目录
#       ./sync.sh pull /tmp/test.aac dump/    ← 指定本地目录
do_pull() {
    local remote_path="$1"
    local local_path="${2:-./}"
    if [ -z "$remote_path" ]; then
        err "用法: $0 pull <远端绝对路径> [本地目录或文件]"
        exit 1
    fi
    log "scp ${DEVICE_HOST}:${remote_path} → ${local_path}"
    # 经编译机中转：因为 Mac 没装 sshpass，复用编译机已经能联通开发板的链路
    ssh -o StrictHostKeyChecking=no ${BUILD_SSH_OPT} "${BUILD_USER}@${BUILD_SSH_TARGET}" "
        sshpass -p '${DEVICE_PASS}' scp -O ${SSH_COMPAT_OPTS} \
            ${DEVICE_USER}@${DEVICE_HOST}:${remote_path} /tmp/_pull_tmp
    "
    scp -o StrictHostKeyChecking=no ${BUILD_SSH_OPT} \
        "${BUILD_USER}@${BUILD_SSH_TARGET}:/tmp/_pull_tmp" "${local_path}"
    ssh -o StrictHostKeyChecking=no ${BUILD_SSH_OPT} "${BUILD_USER}@${BUILD_SSH_TARGET}" "rm -f /tmp/_pull_tmp"
    log "拉取完成"
}

# ---------- 远端清理 ----------
do_clean() {
    warn "清理 ${BUILD_HOST}:${BUILD_DIR}/build & dist"
    ssh -o StrictHostKeyChecking=no ${BUILD_SSH_OPT} "${BUILD_USER}@${BUILD_SSH_TARGET}" "cd ${BUILD_DIR} && rm -rf build dist"
    log "清理完成"
}

# ---------- 看开发板日志 ----------
# 进入前先清理可能残留的 tail 进程（Ctrl+C 关闭 ssh 时 BusyBox tail 不一定收得到 HUP）
do_log() {
    log "tail 开发板日志（Ctrl+C 退出，远端 tail 会随会话一起结束）"
    # -tt 强制分配 PTY，使本地 Ctrl+C 直接转发为 SIGINT/HUP，并在 ssh 关闭时由内核回收远端进程
    # 进入 tail 前先 killall 残留的 tail，防止越积越多
    ssh -t -o StrictHostKeyChecking=no ${BUILD_SSH_OPT} "${BUILD_USER}@${BUILD_SSH_TARGET}" "
        sshpass -p '${DEVICE_PASS}' ssh -tt \
            ${SSH_COMPAT_OPTS} \
            ${DEVICE_USER}@${DEVICE_HOST} \
            'killall tail 2>/dev/null; exec tail -f ${DEVICE_DIR}/camera.log'
    "
}

# ---------- 入口 ----------
case "${1:-sync}" in
    sync)   do_sync ;;
    build)  do_sync && do_build ;;
    push)   do_sync && do_build && do_push_device ;;
    run)    do_push_device ;;
    pull)   shift; do_pull "$@" ;;
    clean)  do_clean ;;
    log)    do_log ;;
    *)
        err "未知命令: $1"
        cat <<EOF
用法:
  $0 sync                       # 仅同步源码 (Mac → 编译机)
  $0 build                      # 同步 + 远端编译
  $0 push                       # 同步 + 编译 + 推送到开发板 + 启动
  $0 run                        # 跳过编译, 仅推送当前编译产物到开发板
  $0 pull <remote> [localdir]   # 从开发板拉文件回 Mac (经编译机中转)
  $0 clean                      # 清理编译机的 build/dist
  $0 log                        # 查看开发板 camera 运行日志
EOF
        exit 1
        ;;
esac
