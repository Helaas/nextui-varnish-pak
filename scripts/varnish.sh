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

VARNISH_FIFO="/tmp/varnish.fifo"

# Show a pill notification.
# Args: <client_id> <position> <duration_secs> <text>
varnish_pill() {
    echo "PILL $1 $2 $3 $4" > "$VARNISH_FIFO" 2>/dev/null
}

# Hide the pill for the given client.
# Args: <client_id>
varnish_hide() {
    echo "HIDE $1" > "$VARNISH_FIFO" 2>/dev/null
}

# Clear all overlay pills from all clients.
varnish_clear() {
    echo "CLEAR" > "$VARNISH_FIFO" 2>/dev/null
}
