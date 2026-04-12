#!/bin/sh
set -eu

TMP_ROOT=$(mktemp -d)
trap 'rm -rf "$TMP_ROOT"' EXIT

cc -std=gnu11 -O0 -g -Wall -Wextra -Werror \
    -Isrc \
    tests/preload_capture_tests.c \
    -o "$TMP_ROOT/preload_capture_tests"

"$TMP_ROOT/preload_capture_tests"
