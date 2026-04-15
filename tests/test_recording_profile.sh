#!/bin/sh
set -eu

TMP_ROOT=$(mktemp -d)
trap 'rm -rf "$TMP_ROOT"' EXIT

cc -std=gnu11 -O2 -Wall -Wextra -Werror \
    -Isrc \
    tests/recording_profile_tests.c src/recording_profile.c \
    -lm \
    -o "$TMP_ROOT/recording_profile_tests"

"$TMP_ROOT/recording_profile_tests"
