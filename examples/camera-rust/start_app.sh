#!/bin/sh

APP_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
export LD_LIBRARY_PATH="$APP_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

VARIANT="${CAMERA_VARIANT:-$(cat "$APP_DIR/active_variant" 2>/dev/null)}"
case "$VARIANT" in
    rust) SOURCE="$APP_DIR/camera_rust" ;;
    cpp)  SOURCE="$APP_DIR/camera_cpp" ;;
    *)
        echo "invalid or missing active_variant: $VARIANT" >&2
        exit 1
        ;;
esac

[ -x "$SOURCE" ] || {
    echo "camera variant not found: $SOURCE" >&2
    exit 1
}

mkdir -p "$APP_DIR/state/tls" \
         "$APP_DIR/state/acme-webroot/.well-known/acme-challenge"
chmod 0700 "$APP_DIR/state/tls"

# Keep the active executable name "camera" so pidof/killall/watchdog remain
# compatible with the original board service.
if [ ! -x "$APP_DIR/camera" ] || ! cmp -s "$SOURCE" "$APP_DIR/camera"; then
    cp "$SOURCE" "$APP_DIR/camera.new" || exit 1
    chmod +x "$APP_DIR/camera.new"
    mv "$APP_DIR/camera.new" "$APP_DIR/camera"
fi

exec "$APP_DIR/camera"
