#!/usr/bin/env bash
# ============================================================
# debugcore.sh —— 从开发板拉取 core + run.log，用交叉 gdb 调试
#
# 用法：
#   ./debugcore.sh              # 拉取 core + run.log 到 dist/ 并启动 gdb
#   ./debugcore.sh pull         # 只拉取，不启动 gdb
# ============================================================
set -e

# ---------- 配置（与 sync.sh 保持一致） ----------
DEVICE_USER="root"
DEVICE_HOST="192.168.1.13"
DEVICE_DIR="/root/maix_dist"
DEVICE_PASS="root"

GDB_TOOLCHAIN="/opt/toolchain-sunxi-musl/toolchain/bin/arm-openwrt-linux-muslgnueabi-gdb"

SSH_COMPAT_OPTS="-o StrictHostKeyChecking=no \
                 -o UserKnownHostsFile=/dev/null \
                 -o HostKeyAlgorithms=+ssh-rsa \
                 -o PubkeyAcceptedAlgorithms=+ssh-rsa \
                 -o KexAlgorithms=+diffie-hellman-group1-sha1,diffie-hellman-group14-sha1"

mkdir -p dist

echo "==> Pulling core and run.log from ${DEVICE_HOST}..."
sshpass -p "${DEVICE_PASS}" scp ${SSH_COMPAT_OPTS} \
    "${DEVICE_USER}@${DEVICE_HOST}:${DEVICE_DIR}/core" \
    "${DEVICE_USER}@${DEVICE_HOST}:${DEVICE_DIR}/run.log" \
    dist/

echo "==> Done. core & run.log in dist/"

if [ "${1:-}" = "pull" ]; then
    exit 0
fi

if [ ! -x "${GDB_TOOLCHAIN}" ]; then
    echo "WARN: gdb not found at ${GDB_TOOLCHAIN}"
    echo "      请修改脚本中的 GDB_TOOLCHAIN 路径，或手动运行 gdb。"
    exit 1
fi

echo "==> Launching gdb..."
exec "${GDB_TOOLCHAIN}" dist/camera dist/core