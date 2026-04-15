#!/bin/sh
set -eu

TMP_ROOT=$(mktemp -d)
trap 'rm -rf "$TMP_ROOT"' EXIT

cc -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter \
    -Isrc \
    tests/recording_transport_tests.c \
    -o "$TMP_ROOT/recording_transport_tests"

"$TMP_ROOT/recording_transport_tests"
