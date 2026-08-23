#!/bin/sh
set -e

APP_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"

status() {
    variant=$(cat "$APP_DIR/active_variant" 2>/dev/null || echo unknown)
    pid=$(pidof camera 2>/dev/null || true)
    echo "active_variant=$variant"
    echo "pid=${pid:-stopped}"
    if [ -n "$pid" ]; then
        echo "exe=$(readlink /proc/$pid/exe 2>/dev/null || true)"
        awk '/^(VmRSS|VmData|Threads):/{print}' "/proc/$pid/status" 2>/dev/null || true
    fi
}

case "${1:-status}" in
    status)
        status
        exit 0
        ;;
    rust|cpp)
        variant="$1"
        ;;
    *)
        echo "usage: $0 [status|rust|cpp]" >&2
        exit 2
        ;;
esac

source="$APP_DIR/camera_$variant"
[ -x "$source" ] || {
    echo "camera variant not found: $source" >&2
    exit 1
}

old_variant=$(cat "$APP_DIR/active_variant" 2>/dev/null || true)

killall -9 camera 2>/dev/null || true
for _ in 1 2 3 4 5 6 7 8 9 10; do
    pidof camera >/dev/null 2>&1 || break
    sleep 1
done
sleep 2

case "$old_variant" in
    rust|cpp)
        [ -f "$APP_DIR/camera.log" ] &&
            mv -f "$APP_DIR/camera.log" "$APP_DIR/camera_${old_variant}.log"
        ;;
esac
rm -f "$APP_DIR/camera.log"

printf '%s\n' "$variant" > "$APP_DIR/active_variant.tmp"
mv "$APP_DIR/active_variant.tmp" "$APP_DIR/active_variant"

cd "$APP_DIR"
(trap "" HUP; CAMERA_VARIANT="$variant" ./start_app.sh </dev/null >camera.log 2>&1) &
sleep 3

pid=$(pidof camera 2>/dev/null || true)
if [ -z "$pid" ]; then
    echo "camera $variant failed to start; see camera.log" >&2
    exit 1
fi

echo "camera switched to $variant (pid=$pid)"
