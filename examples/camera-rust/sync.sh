#!/usr/bin/env bash
# Mac -> build host -> V831 deployment helper for camera-rust.
set -e

BUILD_USER="root"
BUILD_HOST="${BUILD_HOST:-lecoo.gwghome.site}"
BUILD_DIR="/root/work/libmaix/examples/camera-rust"
CAMERA_BUILD_DIR="/root/work/libmaix/examples/camera"
WEBRTC_BUILD_DIR="/root/work/github/webrtc"
WEBRTC_REV="a91689c3dd237ea48a0ce5a827a69d3807420a5c"

DEVICE_USER="root"
DEVICE_HOST="192.168.1.13"
DEVICE_DIR="/root/maix_dist"
DEVICE_PASS="root"

HUB_USER="${HUB_USER:-android}"
HUB_HOST="${HUB_HOST:-mi6.gwghome.site}"
HUB_EDGE_KEY="${HUB_EDGE_KEY:-/home/android/.ssh/camera-hub-edge-acme-rsa}"

TOOLCHAIN_DIR="/opt/toolchain-sunxi-musl/toolchain/bin"
RUST_TARGET="armv7-unknown-linux-musleabihf"
LOCAL_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CAMERA_LOCAL_DIR="$(cd "${LOCAL_DIR}/../camera" && pwd)"
WEBRTC_LOCAL_DIR="$(cd "${LOCAL_DIR}/../../../github/webrtc" && pwd)"

SSH_COMPAT_OPTS="-o StrictHostKeyChecking=no \
                 -o UserKnownHostsFile=/dev/null \
                 -o HostKeyAlgorithms=+ssh-rsa \
                 -o PubkeyAcceptedAlgorithms=+ssh-rsa \
                 -o KexAlgorithms=+diffie-hellman-group1-sha1,diffie-hellman-group14-sha1"

if [[ "${BUILD_HOST}" == *:* ]]; then
    BUILD_SSH_TARGET="build6"
    BUILD_SSH_OPT="-o HostName=${BUILD_HOST}"
else
    BUILD_SSH_TARGET="${BUILD_HOST}"
    BUILD_SSH_OPT=""
fi

GREEN='\033[0;32m'; YELLOW='\033[1;33m'; RED='\033[0;31m'; NC='\033[0m'
log()  { echo -e "${GREEN}[rust-sync]${NC} $*"; }
warn() { echo -e "${YELLOW}[warn]${NC} $*"; }
err()  { echo -e "${RED}[err]${NC} $*" >&2; }

rsync_common=(
    -avz --delete
    --exclude=.git/
    --exclude=target/
    --exclude=dist/
    --exclude=build/
    --exclude=.cache/
    --exclude=.DS_Store
)

do_sync() {
    local webrtc_revision
    webrtc_revision=$(git -C "${WEBRTC_LOCAL_DIR}" rev-parse HEAD)
    if [ "${webrtc_revision}" != "${WEBRTC_REV}" ]; then
        err "本地 webrtc-rs 版本不匹配: ${webrtc_revision}"
        err "期望固定版本: ${WEBRTC_REV}"
        exit 1
    fi

    log "同步 camera-rust -> ${BUILD_HOST}:${BUILD_DIR}"
    rsync "${rsync_common[@]}" \
        -e "ssh -o StrictHostKeyChecking=no ${BUILD_SSH_OPT}" \
        "${LOCAL_DIR}/" "${BUILD_USER}@${BUILD_SSH_TARGET}:${BUILD_DIR}/"

    # The native bridge compiles selected, current camera modules directly.
    log "同步 camera C++ reference -> ${BUILD_HOST}:${CAMERA_BUILD_DIR}"
    rsync "${rsync_common[@]}" \
        --exclude=record/ --exclude=snapshot/ \
        -e "ssh -o StrictHostKeyChecking=no ${BUILD_SSH_OPT}" \
        "${CAMERA_LOCAL_DIR}/" \
        "${BUILD_USER}@${BUILD_SSH_TARGET}:${CAMERA_BUILD_DIR}/"

    # Cargo.toml points at the public fork/revision. The build host cannot
    # reach GitHub, so mirror that exact revision as an offline Cargo patch.
    log "同步 webrtc-rs 离线镜像 -> ${BUILD_HOST}:${WEBRTC_BUILD_DIR}"
    ssh -o StrictHostKeyChecking=no ${BUILD_SSH_OPT} \
        "${BUILD_USER}@${BUILD_SSH_TARGET}" "mkdir -p ${WEBRTC_BUILD_DIR}"
    rsync "${rsync_common[@]}" \
        --exclude=output.h264 --exclude=output.ogg \
        -e "ssh -o StrictHostKeyChecking=no ${BUILD_SSH_OPT}" \
        "${WEBRTC_LOCAL_DIR}/" \
        "${BUILD_USER}@${BUILD_SSH_TARGET}:${WEBRTC_BUILD_DIR}/"

}

do_build() {
    log "重建 camera native dependencies + Rust release"
    ssh -o StrictHostKeyChecking=no ${BUILD_SSH_OPT} \
        "${BUILD_USER}@${BUILD_SSH_TARGET}" "
        set -e
        source /root/.cargo/env
        command -v cargo >/dev/null || {
            echo 'cargo not found; install rustup for root first' >&2
            exit 1
        }
        rustup target add ${RUST_TARGET}

        cd ${CAMERA_BUILD_DIR}
        python3 project.py build

        cd ${BUILD_DIR}
        export LIBMAIX_SDK_PATH=/root/work/libmaix
        export CAMERA_CPP_DIR=${CAMERA_BUILD_DIR}
        export CAMERA_BUILD_DIR=${CAMERA_BUILD_DIR}/build
        export V831_TOOLCHAIN_PATH=${TOOLCHAIN_DIR}
        export STAGING_DIR=/opt/toolchain-sunxi-musl
        export CC_armv7_unknown_linux_musleabihf=${TOOLCHAIN_DIR}/arm-openwrt-linux-muslgnueabi-gcc
        export CXX_armv7_unknown_linux_musleabihf=${TOOLCHAIN_DIR}/arm-openwrt-linux-muslgnueabi-g++
        export AR_armv7_unknown_linux_musleabihf=${TOOLCHAIN_DIR}/arm-openwrt-linux-muslgnueabi-ar
        cargo build --release \
            --config 'patch.\"https://github.com/gengwenguan/webrtc\".webrtc.path=\"${WEBRTC_BUILD_DIR}/webrtc\"'

        rm -rf dist
        mkdir -p dist
        cp target/${RUST_TARGET}/release/camera-rust dist/camera_rust
        cp ${CAMERA_BUILD_DIR}/dist/camera dist/camera_cpp
        cp start_app.sh dist/start_app.sh
        cp switch_camera.sh dist/switch_camera.sh
        chmod +x dist/camera_rust dist/camera_cpp \
                 dist/start_app.sh dist/switch_camera.sh
        for asset in lib web prompt mem_watchdog.sh; do
            if [ -e ${CAMERA_BUILD_DIR}/dist/\${asset} ]; then
                cp -a ${CAMERA_BUILD_DIR}/dist/\${asset} dist/
            fi
        done
    "
    log "构建完成：dist/camera_rust + dist/camera_cpp"
}

provision_hub_edge_key() {
    log "安装 Camera-hub ACME SSH 公钥到开发板"
    local public_key
    public_key=$(ssh -6 -o StrictHostKeyChecking=no \
        "${HUB_USER}@${HUB_HOST}" \
        "test -r '${HUB_EDGE_KEY}.pub' && cat '${HUB_EDGE_KEY}.pub'") || {
        warn "Camera-hub ACME 公钥尚未生成，先部署 camera-hub 后再重试"
        return 0
    }
    ssh -o StrictHostKeyChecking=no ${BUILD_SSH_OPT} \
        "${BUILD_USER}@${BUILD_SSH_TARGET}" "
        sshpass -p '${DEVICE_PASS}' ssh ${SSH_COMPAT_OPTS} \
            ${DEVICE_USER}@${DEVICE_HOST} \
            'mkdir -p /etc/dropbear;
             touch /etc/dropbear/authorized_keys;
             grep -Fqx \"${public_key}\" /etc/dropbear/authorized_keys ||
                 echo \"${public_key}\" >> /etc/dropbear/authorized_keys;
             chmod 600 /etc/dropbear/authorized_keys'
    "
    ssh -6 -o StrictHostKeyChecking=no "${HUB_USER}@${HUB_HOST}" "
        sed -i \"s/^CAMERA_HUB_EDGE_ACME_ENABLED=.*/CAMERA_HUB_EDGE_ACME_ENABLED='true'/\" \
            /home/android/.config/camera-hub.env
        nohup /usr/local/bin/camera-hub-acme-edge \
            >> /home/android/camera-hub-acme-edge.log 2>&1 &
    "
}

do_push() {
    log "推送 C++/Rust 双版本到 ${DEVICE_HOST}:${DEVICE_DIR}"
    ssh -o StrictHostKeyChecking=no ${BUILD_SSH_OPT} \
        "${BUILD_USER}@${BUILD_SSH_TARGET}" "
        set -e
        cd ${BUILD_DIR}
        test -x dist/camera_rust
        test -x dist/camera_cpp

        sshpass -p '${DEVICE_PASS}' ssh ${SSH_COMPAT_OPTS} \
            ${DEVICE_USER}@${DEVICE_HOST} \
            'killall -9 camera 2>/dev/null || true;
             # BusyBox runs shell scripts as "sh ./mem_watchdog.sh", so
             # killall by script name misses old watchdog instances.
             for proc in /proc/[0-9]*; do
                 [ -r "\$proc/cmdline" ] || continue
                 cmd=\$(cat "\$proc/cmdline" 2>/dev/null)
                 cwd=\$(readlink "\$proc/cwd" 2>/dev/null)
                 case "\$cwd" in
                     /root/maix_dist)
                         case "\$cmd" in
                             *mem_watchdog.sh*) kill -9 "\${proc##*/}" 2>/dev/null || true ;;
                         esac
                         ;;
                 esac
             done;
             for i in 1 2 3 4 5 6 7 8 9 10; do
                 pidof camera >/dev/null 2>&1 || break
                 sleep 1
             done
             sleep 2
             mkdir -p ${DEVICE_DIR}
             mkdir -p ${DEVICE_DIR}/record ${DEVICE_DIR}/snapshot
             mkdir -p ${DEVICE_DIR}/state/tls \
                 ${DEVICE_DIR}/state/acme-webroot/.well-known/acme-challenge
             if [ ! -s ${DEVICE_DIR}/state/tls/fullchain.pem ] &&
                [ -s ${DEVICE_DIR}/cert/server.crt ] &&
                [ -s ${DEVICE_DIR}/cert/server.key ]; then
                 cp ${DEVICE_DIR}/cert/server.crt ${DEVICE_DIR}/state/tls/fullchain.pem
                 cp ${DEVICE_DIR}/cert/server.key ${DEVICE_DIR}/state/tls/private.key
                 chmod 600 ${DEVICE_DIR}/state/tls/private.key
             fi
             # Preserve shared config/actions/record/snapshot across updates.
             rm -rf ${DEVICE_DIR}/lib ${DEVICE_DIR}/web \
                    ${DEVICE_DIR}/cert ${DEVICE_DIR}/prompt
             rm -f ${DEVICE_DIR}/camera ${DEVICE_DIR}/start_app.sh \
                   ${DEVICE_DIR}/switch_camera.sh ${DEVICE_DIR}/mem_watchdog.sh \
                   ${DEVICE_DIR}/camera_cpp ${DEVICE_DIR}/camera_rust'

        sshpass -p '${DEVICE_PASS}' scp -r -O ${SSH_COMPAT_OPTS} \
            dist/. ${DEVICE_USER}@${DEVICE_HOST}:${DEVICE_DIR}/

        sshpass -p '${DEVICE_PASS}' ssh ${SSH_COMPAT_OPTS} \
            ${DEVICE_USER}@${DEVICE_HOST} \
            'cd ${DEVICE_DIR};
             ./switch_camera.sh rust;
             rm -f watchdog.log;
             if [ -x ./mem_watchdog.sh ]; then
                 (trap \"\" HUP; sleep 30; ./mem_watchdog.sh </dev/null >/dev/null 2>&1) &
             fi
             sleep 1'
    "
    provision_hub_edge_key
    log "双版本部署完成，当前运行 Rust"
}

do_switch() {
    local variant="$1"
    case "${variant}" in
        rust|cpp) ;;
        *) err "用法: $0 switch <rust|cpp>"; exit 2 ;;
    esac
    ssh -o StrictHostKeyChecking=no ${BUILD_SSH_OPT} \
        "${BUILD_USER}@${BUILD_SSH_TARGET}" "
        sshpass -p '${DEVICE_PASS}' ssh ${SSH_COMPAT_OPTS} \
            ${DEVICE_USER}@${DEVICE_HOST} \
            'cd ${DEVICE_DIR} && ./switch_camera.sh ${variant}'
    "
}

do_status() {
    ssh -o StrictHostKeyChecking=no ${BUILD_SSH_OPT} \
        "${BUILD_USER}@${BUILD_SSH_TARGET}" "
        sshpass -p '${DEVICE_PASS}' ssh ${SSH_COMPAT_OPTS} \
            ${DEVICE_USER}@${DEVICE_HOST} \
            'cd ${DEVICE_DIR} && ./switch_camera.sh status'
    "
}

do_log() {
    ssh -t -o StrictHostKeyChecking=no ${BUILD_SSH_OPT} \
        "${BUILD_USER}@${BUILD_SSH_TARGET}" "
        sshpass -p '${DEVICE_PASS}' ssh -tt ${SSH_COMPAT_OPTS} \
            ${DEVICE_USER}@${DEVICE_HOST} \
            'exec tail -f ${DEVICE_DIR}/camera.log'
    "
}

do_clean() {
    warn "清理远端 Rust target/dist"
    ssh -o StrictHostKeyChecking=no ${BUILD_SSH_OPT} \
        "${BUILD_USER}@${BUILD_SSH_TARGET}" \
        "cd ${BUILD_DIR} && rm -rf target dist"
}

case "${1:-sync}" in
    sync)  do_sync ;;
    build) do_sync && do_build ;;
    push)  do_sync && do_build && do_push ;;
    run)   do_push ;;
    switch) shift; do_switch "${1:-}" ;;
    status) do_status ;;
    log)   do_log ;;
    clean) do_clean ;;
    *)
        err "未知命令: $1"
        cat <<EOF
用法:
  $0 sync    # 同步 Rust 与 camera 原生参考源码
  $0 build   # 同步并交叉编译，组装 dist
  $0 push    # 编译、推送到开发板并启动
  $0 run     # 推送远端已有 dist
  $0 switch rust|cpp  # 在共享目录中切换运行版本
  $0 status  # 查看当前运行版本
  $0 log     # 查看 Rust camera 日志
  $0 clean   # 清理远端 Rust 构建产物
EOF
        exit 1
        ;;
esac
