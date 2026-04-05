#!/bin/sh
set -eu

APP_BIN="varnish"
PAK_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PAK_NAME=$(basename "$PAK_DIR")
PAK_NAME=${PAK_NAME%.pak}

cd "$PAK_DIR"

# Ensure LD_LIBRARY_PATH includes system libs
export LD_LIBRARY_PATH="/usr/trimui/lib:$PAK_DIR/lib:${LD_LIBRARY_PATH:-}"

if [ -n "${SHARED_USERDATA_PATH:-}" ]; then
    SHARED_USERDATA_ROOT="$SHARED_USERDATA_PATH"
elif [ -d "/mnt/SDCARD/.userdata/shared" ] || [ -d "/mnt/SDCARD" ]; then
    SHARED_USERDATA_ROOT="/mnt/SDCARD/.userdata/shared"
else
    SHARED_USERDATA_ROOT="${HOME:-/tmp}/.userdata/shared"
fi
LOG_ROOT=${LOGS_PATH:-"$SHARED_USERDATA_ROOT/logs"}
mkdir -p "$LOG_ROOT"
LOG_FILE="$LOG_ROOT/$APP_BIN.txt"
: >"$LOG_FILE"

exec >>"$LOG_FILE"
exec 2>&1

echo "=== Launching $PAK_NAME ($APP_BIN) at $(date) ==="
echo "platform=${PLATFORM:-unknown} device=${DEVICE:-unknown}"

exec "./$APP_BIN" --ui
