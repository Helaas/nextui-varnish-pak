# Bundled ffmpeg Runtime

Varnish now builds ffmpeg from source during packaging.

Source download/build:

- `scripts/build_ffmpeg.sh ensure-ffmpeg <platform>` downloads the pinned upstream tarball, verifies its SHA-256, and cross-builds a minimal ffmpeg CLI
- build artifacts land under `build/third_party/<platform>/ffmpeg/`
- the staged runtime copied into the pak lives under `build/third_party/<platform>/ffmpeg/package/`

Make targets:

- `make ffmpeg-tg5040`
- `make ffmpeg-tg5050`
- `make ffmpeg-my355`
- `make ffmpeg-all`

Packaging pulls `build/third_party/<platform>/ffmpeg/package/bin/ffmpeg` into `Varnish.pak/bin/ffmpeg`.
