#!/bin/sh
set -eu

RELEASE_ZIP=${1:?usage: test_hooks.sh <NextUI release zip>}
TMP_ROOT=$(mktemp -d)
trap 'rm -rf "$TMP_ROOT"' EXIT

SDCARD_ROOT="$TMP_ROOT/sdcard"
mkdir -p "$SDCARD_ROOT/.tmp_update"

unzip -p "$RELEASE_ZIP" trimui/app/.tmp_update/tg5040.sh > "$SDCARD_ROOT/.tmp_update/tg5040.sh"
unzip -p "$RELEASE_ZIP" trimui/app/.tmp_update/tg5050.sh > "$SDCARD_ROOT/.tmp_update/tg5050.sh"

cc -std=gnu11 -O0 -g -Wall -Wextra -Werror \
    -Isrc \
    tests/hooks_tests.c src/hooks.c src/device.c \
    -o "$TMP_ROOT/hooks_tests"

"$TMP_ROOT/hooks_tests" "$TMP_ROOT"
