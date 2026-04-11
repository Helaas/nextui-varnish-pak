#!/bin/sh
set -eu

TMP_ROOT=$(mktemp -d)
trap 'rm -rf "$TMP_ROOT"' EXIT

SDL_CFLAGS=$(pkg-config --cflags sdl2 SDL2_image 2>/dev/null || true)
SDL_LIBS=$(pkg-config --libs sdl2 SDL2_image 2>/dev/null || true)

cc -std=gnu11 -O0 -g -Wall -Wextra -Werror \
    -Isrc \
    ${SDL_CFLAGS} \
    tests/hotkeys_tests.c src/hotkeys.c src/device.c src/manual.c src/screenshot.c \
    ${SDL_LIBS} \
    -o "$TMP_ROOT/hotkeys_tests"

"$TMP_ROOT/hotkeys_tests" "$TMP_ROOT"
