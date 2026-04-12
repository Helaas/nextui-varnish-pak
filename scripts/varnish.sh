#!/bin/sh
# varnish.sh — Shell client helper for Varnish overlay system.
#
# Source this file in your Pak's scripts, or copy the functions you need.
#
# Usage:
#   . /mnt/SDCARD/Tools/${PLATFORM}/Varnish.pak/scripts/varnish.sh
#
#   varnish_pill "mypak" "bottom-center" 5 "Download complete!"
#   varnish_pill "mypak" "top-right" 0 "Syncing..."
#   varnish_hide "mypak"
#   varnish_clear
#
# Positions: top-left, top-center, top-right,
#            bottom-left, bottom-center, bottom-right
#
# If the Varnish daemon is not running, calls fail silently.
# The helper prefers the bundled `varnish` binary because it opens the FIFO
# with O_NONBLOCK. The raw shell fallback only runs when that binary is not
# available, and it guards the write with the daemon PID file first.

VARNISH_FIFO="/tmp/varnish.fifo"
VARNISH_PID="/tmp/varnish.pid"
VARNISH_ROOT="${VARNISH_ROOT:-${SDCARD_PATH:-/mnt/SDCARD}/Tools/${PLATFORM:-tg5040}/Varnish.pak}"
VARNISH_BIN="${VARNISH_BIN:-$VARNISH_ROOT/varnish}"

varnish_send_raw() {
    [ -x "$VARNISH_BIN" ] || return 1
    "$VARNISH_BIN" "$@" >/dev/null 2>&1
}

varnish_send_fallback() {
    [ -p "$VARNISH_FIFO" ] || return 0
    [ -r "$VARNISH_PID" ] || return 0

    pid=$(cat "$VARNISH_PID" 2>/dev/null) || return 0
    [ -n "$pid" ] || return 0
    kill -0 "$pid" 2>/dev/null || return 0

    printf '%s\n' "$1" > "$VARNISH_FIFO" 2>/dev/null || true
}

# Show a pill notification.
# Args: <client_id> <position> <duration_secs> <text>
varnish_pill() {
    varnish_send_raw --pill "$1" "$2" "$3" "$4" || \
        varnish_send_fallback "PILL $1 $2 $3 $4"
}

# Hide the pill for the given client.
# Args: <client_id>
varnish_hide() {
    varnish_send_raw --hide "$1" || varnish_send_fallback "HIDE $1"
}

# Clear all overlay pills from all clients.
varnish_clear() {
    varnish_send_raw --clear || varnish_send_fallback "CLEAR"
}
