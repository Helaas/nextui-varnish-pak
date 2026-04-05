#!/bin/sh
set -eu
PAK_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$PAK_DIR"

# Ensure LD_LIBRARY_PATH includes system libs
export LD_LIBRARY_PATH="/usr/trimui/lib:$PAK_DIR/lib:${LD_LIBRARY_PATH:-}"

# Start daemon if not already running
if [ ! -f /tmp/varnish.pid ] || ! kill -0 "$(cat /tmp/varnish.pid 2>/dev/null)" 2>/dev/null; then
    ./varnish --daemon &
    sleep 0.3
fi

# Install hooks if not yet installed
./varnish --install 2>/dev/null || true
