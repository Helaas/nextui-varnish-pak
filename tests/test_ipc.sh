#!/bin/sh
set -eu

TMP_ROOT=$(mktemp -d)
trap 'rm -rf "$TMP_ROOT"' EXIT

FIFO_PATH="$TMP_ROOT/varnish.fifo"
PID_PATH="$TMP_ROOT/varnish.pid"

cc -std=gnu11 -O0 -g -Wall -Wextra -Werror \
    -Isrc -Iinclude \
    -DVARNISH_FIFO_PATH="\"$FIFO_PATH\"" \
    -DVARNISH_PID_PATH="\"$PID_PATH\"" \
    -DVARNISH_FIFO="\"$FIFO_PATH\"" \
    tests/ipc_tests.c src/ipc.c \
    -o "$TMP_ROOT/ipc_tests"

"$TMP_ROOT/ipc_tests"
