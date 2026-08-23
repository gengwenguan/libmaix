#!/bin/sh
# ============================================================
# mem_watchdog.sh —— camera 进程内存水位看门狗
#
# 背景：camera 依赖的闭源库（NPU 驱动 + ffmpeg/cedar VE）存在慢速内存泄漏，
#   AI 开 ~0.7MB/h、AI 关 ~0.28MB/h，连续运行数天后 VmData 涨到 ~140MB，
#   吃光 60MB 内存 + 128MB swap，触发 ENOMEM（提示音 play failed / 画面黑屏）。
#   根因在改不动的闭源库，故用看门狗管理：内存到危险区前自动重启拉回基线(~18MB)。
#
# 逻辑：每 CHECK_INTERVAL 秒读一次 /proc/<pid>/status 的 VmData，
#   超过 THRESHOLD_KB 就杀掉 camera 并重新拉起（复用 start_app.sh）。
#   重启沿用 sync.sh 验证过的 V831 独占设备回收流程（cedar/VI/disp/snd
#   异步释放需要时间，否则新进程卡死）。
#
# 部署：随项目放在 <exe_dir>/，由 S02app 开机拉起（见 install 段注释）。
# 日志：<exe_dir>/watchdog.log
# ============================================================

DIST_DIR="$(cd "$(dirname "$0")"; pwd)"
START_APP="$DIST_DIR/start_app.sh"
LOG="$DIST_DIR/watchdog.log"

# ---------- 可调参数 ----------
THRESHOLD_KB=40960        # VmData 阈值：40MB（健康基线~18MB，危险区~140MB）
CHECK_INTERVAL=60         # 检查周期（秒）
DRAIN_WAIT=2              # 杀进程后额外等待，让内核异步回收 cedar/video/disp/snd 的 fd
LOG_MAX_LINES=20000       # 日志硬封顶：超过就裁掉前半，防止无界增长撑爆磁盘
OK_LOG_EVERY=5            # 常规 ok 行每 N 次检查才记一条（5*60s=5min），异常/重启总是记

# 北京时间：板上系统时区是 UTC 且无 /etc/localtime，date 默认出 UTC。
# 用 TZ="UTC-8" 强制按东八区输出（POSIX 里 UTC-8 表示比 UTC 快 8h）。
# 与 recorder.cpp 里 +8h 的处理保持一致。
now_bj() {
    TZ="UTC-8" date '+%Y-%m-%d %H:%M:%S'
}

# 日志轮转：超过 LOG_MAX_LINES 行就只保留后半，原地截断。
# 用临时文件 + mv 保证原子，避免截断瞬间被读到半行。
rotate_log() {
    [ -f "$LOG" ] || return 0
    lines=$(wc -l < "$LOG" 2>/dev/null)
    [ -z "$lines" ] && return 0
    if [ "$lines" -gt "$LOG_MAX_LINES" ]; then
        keep=$((LOG_MAX_LINES / 2))
        tail -n "$keep" "$LOG" > "$LOG.tmp" 2>/dev/null && mv "$LOG.tmp" "$LOG"
    fi
}

log() {
    echo "$(now_bj) $*" >> "$LOG"
    rotate_log
}

# 读 camera 当前 VmData（kB）；进程不存在则返回空
read_vmdata() {
    P=$(pidof camera)
    [ -z "$P" ] && return 1
    awk '/^VmData/{print $2}' /proc/"$P"/status 2>/dev/null
}

restart_camera() {
    log "RESTART triggered: VmData=${1}kB > ${THRESHOLD_KB}kB, restarting camera ..."
    killall -9 camera 2>/dev/null
    # 等所有 camera 工作线程退出（最多 10 秒）
    i=0
    while [ $i -lt 10 ]; do
        pidof camera >/dev/null 2>&1 || break
        sleep 1
        i=$((i+1))
    done
    # 额外等待，让内核异步回收独占设备 fd
    sleep "$DRAIN_WAIT"
    # 重新拉起（后台、脱离当前会话）
    cd "$DIST_DIR" || exit 1
    ( trap "" HUP; ./start_app.sh </dev/null >camera.log 2>&1 ) &
    sleep 2
    NEWP=$(pidof camera)
    if [ -n "$NEWP" ]; then
        log "RESTART done: new camera pid=$NEWP"
    else
        log "RESTART WARN: camera not up after restart!"
    fi
}

log "mem_watchdog started (threshold=${THRESHOLD_KB}kB interval=${CHECK_INTERVAL}s)"

MISS=0
OKN=0
while true; do
    VM=$(read_vmdata)
    if [ -z "$VM" ]; then
        # camera 不在：可能正在重启间隙，或异常退出。连续多次缺失则尝试拉起。
        MISS=$((MISS+1))
        log "camera not running (miss=$MISS)"
        if [ "$MISS" -ge 3 ]; then
            log "camera missing >=3 checks, trying to start ..."
            cd "$DIST_DIR" && ( trap "" HUP; ./start_app.sh </dev/null >camera.log 2>&1 ) &
            MISS=0
        fi
        sleep "$CHECK_INTERVAL"
        continue
    fi
    MISS=0
    if [ "$VM" -gt "$THRESHOLD_KB" ]; then
        restart_camera "$VM"
        OKN=0
    else
        # 常规健康日志：每 OK_LOG_EVERY 次才记一条（默认 30min 一条），
        # 避免每分钟一行导致日志无界增长。异常/重启始终记录。
        OKN=$((OKN+1))
        if [ "$OKN" -ge "$OK_LOG_EVERY" ]; then
            log "ok VmData=${VM}kB"
            OKN=0
        fi
    fi
    sleep "$CHECK_INTERVAL"
done
