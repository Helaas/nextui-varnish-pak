#!/bin/sh
set -eu

TMP_ROOT=$(mktemp -d)
trap 'rm -rf "$TMP_ROOT"' EXIT

cc -std=gnu11 -O0 -g -Wall -Wextra -Werror \
    -Isrc \
    tests/recording_session_tests.c \
    src/recording.c src/recording_profile.c src/recording_shm.c src/device.c \
    -o "$TMP_ROOT/recording_session_tests"

"$TMP_ROOT/recording_session_tests"
