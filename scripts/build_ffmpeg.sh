#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage:
  build_ffmpeg.sh ensure-ffmpeg <platform>

Platforms:
  tg5040 | tg5050 | my355
EOF
}

if [[ $# -ne 2 ]]; then
    usage
    exit 1
fi

ACTION="$1"
PLATFORM="$2"

case "$PLATFORM" in
    tg5040|tg5050|my355) ;;
    *)
        echo "Unsupported platform: $PLATFORM" >&2
        exit 1
        ;;
esac

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_ROOT="$REPO_ROOT/build/third_party"
SOURCES_DIR="$BUILD_ROOT/sources"
PLATFORM_ROOT="$BUILD_ROOT/$PLATFORM"
TRIPLET="aarch64-nextui-linux-gnu"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"

FFMPEG_VERSION="7.1.1"
FFMPEG_ARCHIVE="ffmpeg-${FFMPEG_VERSION}.tar.xz"
FFMPEG_URL="https://ffmpeg.org/releases/${FFMPEG_ARCHIVE}"
FFMPEG_SHA256="733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1"

mkdir -p "$SOURCES_DIR" "$PLATFORM_ROOT"

sha256_file() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
        return
    fi
    shasum -a 256 "$1" | awk '{print $1}'
}

download_file() {
    local url="$1"
    local path="$2"

    if command -v curl >/dev/null 2>&1; then
        curl -L --fail --retry 3 -o "$path" "$url"
        return
    fi

    wget -O "$path" "$url"
}

download_and_verify() {
    local url="$1"
    local archive="$2"
    local expected_sha="$3"
    local path="$SOURCES_DIR/$archive"
    local actual_sha

    if [[ ! -f "$path" ]]; then
        echo "[ffmpeg] downloading $archive"
        download_file "$url" "$path"
    fi

    actual_sha="$(sha256_file "$path")"
    if [[ "$actual_sha" != "$expected_sha" ]]; then
        echo "[ffmpeg] checksum mismatch for $archive" >&2
        echo "  expected: $expected_sha" >&2
        echo "  actual:   $actual_sha" >&2
        exit 1
    fi
}

extract_source() {
    local archive="$1"
    local dest="$2"

    rm -rf "$dest"
    mkdir -p "$dest"
    tar -xf "$SOURCES_DIR/$archive" --strip-components=1 -C "$dest"
}

ensure_ffmpeg() {
    local ffmpeg_root="$PLATFORM_ROOT/ffmpeg"
    local src_dir="$ffmpeg_root/src"
    local build_dir="$ffmpeg_root/build"
    local package_dir="$ffmpeg_root/package"
    local stamp="$ffmpeg_root/.stamp-ffmpeg-${FFMPEG_VERSION}-minimal1"

    if [[ -f "$stamp" ]]; then
        return
    fi

    download_and_verify "$FFMPEG_URL" "$FFMPEG_ARCHIVE" "$FFMPEG_SHA256"
    extract_source "$FFMPEG_ARCHIVE" "$src_dir"

    rm -rf "$build_dir" "$package_dir"
    mkdir -p "$build_dir" "$package_dir/bin"

    pushd "$build_dir" >/dev/null
    echo "[ffmpeg] building ffmpeg ${FFMPEG_VERSION} for $PLATFORM"
    "$src_dir/configure" \
        --prefix="$package_dir" \
        --enable-cross-compile \
        --cross-prefix="${TRIPLET}-" \
        --arch=aarch64 \
        --target-os=linux \
        --cc="${TRIPLET}-gcc" \
        --ld="${TRIPLET}-gcc" \
        --ar="${TRIPLET}-ar" \
        --ranlib="${TRIPLET}-ranlib" \
        --strip="${TRIPLET}-strip" \
        --pkg-config=false \
        --disable-autodetect \
        --disable-asm \
        --disable-debug \
        --disable-doc \
        --disable-network \
        --disable-iconv \
        --disable-postproc \
        --disable-swresample \
        --disable-avdevice \
        --disable-ffplay \
        --disable-ffprobe \
        --disable-parsers \
        --disable-bsfs \
        --disable-indevs \
        --disable-outdevs \
        --disable-everything \
        --enable-ffmpeg \
        --enable-protocol=file \
        --enable-protocol=pipe \
        --enable-demuxer=rawvideo \
        --enable-decoder=rawvideo \
        --enable-muxer=avi \
        --enable-encoder=mjpeg \
        --enable-filter=scale \
        --enable-filter=format \
        --enable-small \
        --extra-cflags="-O2 -ffunction-sections -fdata-sections" \
        --extra-ldflags="-Wl,--gc-sections"
    make -j"$JOBS" ffmpeg
    cp -f ffmpeg "$package_dir/bin/ffmpeg"
    "${TRIPLET}-strip" "$package_dir/bin/ffmpeg" || true
    popd >/dev/null

    touch "$stamp"
}

case "$ACTION" in
    ensure-ffmpeg)
        ensure_ffmpeg
        ;;
    *)
        usage
        exit 1
        ;;
esac
