# Varnish

Varnish is a shared overlay service for NextUI Paks. It intercepts SDL's render pipeline via `LD_PRELOAD` and composites notification pills directly into NextUI's frames — no framebuffer fighting, no flicker.

Other Paks send a single line to a FIFO to show or hide a pill. That's the entire API.

---

## How it works

Varnish runs a background daemon that:
1. Listens on `/tmp/varnish.fifo` for overlay commands
2. Renders pills (rounded rect + text) into shared memory slots
3. The `varnish_overlay.so` preload hook reads those slots and composites them into NextUI's SDL renderer before each frame is presented

Up to 8 overlay slots can be active simultaneously — one per client Pak.

---

## Integrating your Pak

### Shell (simplest)

Write a single line to the FIFO:

```sh
echo "PILL mypak bottom-center 5 Scraping artwork..." > /tmp/varnish.fifo
```

To hide it before it expires:

```sh
echo "HIDE mypak" > /tmp/varnish.fifo
```

Or source the helper script bundled with Varnish:

```sh
. /mnt/SDCARD/Tools/${PLATFORM}/Varnish.pak/scripts/varnish.sh

varnish_pill "mypak" "bottom-center" 5 "Scraping artwork..."
varnish_hide "mypak"
```

### C

Copy `include/varnish.h` from the Varnish.pak directory into your project (or reference it directly). It's header-only — no linking required.

```c
#include "varnish.h"

// Show a pill for 5 seconds
varnish_pill("mypak", "bottom-center", 5, "Scraping artwork...");

// Show indefinitely (duration 0 = stays until hidden)
varnish_pill("mypak", "top-right", 0, "Syncing...");

// Hide it
varnish_hide("mypak");
```

The header is in `Varnish.pak/include/varnish.h` on the device at:
```
/mnt/SDCARD/Tools/{PLATFORM}/Varnish.pak/include/varnish.h
```

---

## Command reference

All commands are newline-terminated text written to `/tmp/varnish.fifo`.

| Command | Description |
|---|---|
| `PILL <client_id> <position> <duration_secs> <text>` | Show a pill |
| `HIDE <client_id>` | Hide the pill for this client |
| `CLEAR` | Hide all pills from all clients |
| `QUIT` | Stop the Varnish daemon |

**`client_id`** — A short alphanumeric identifier for your Pak (e.g. `menulody`, `scrapegoat`). Each client gets one slot. Sending `PILL` again with the same client_id replaces the existing pill.

**`position`** — Where the pill appears on screen:

| Value | Location |
|---|---|
| `top-left` | Top-left corner |
| `top-center` | Top edge, centered |
| `top-right` | Top-right corner |
| `bottom-left` | Bottom-left corner |
| `bottom-center` | Bottom edge, centered |
| `bottom-right` | Bottom-right corner |

**`duration_secs`** — How long the pill stays visible. `0` means indefinite — it stays until you send `HIDE`.

**`text`** — The text displayed in the pill. Long text is automatically truncated with `...` to fit.

### Examples

```sh
# Show for 3 seconds at bottom-center
echo "PILL scrapegoat bottom-center 3 Artwork downloaded!" > /tmp/varnish.fifo

# Show indefinitely at top-right
echo "PILL shortcuts top-right 0 Syncing resume state..." > /tmp/varnish.fifo

# Hide it when done
echo "HIDE shortcuts" > /tmp/varnish.fifo

# Clear everything
echo "CLEAR" > /tmp/varnish.fifo
```

---

## Graceful degradation

All IPC calls are non-blocking and fail silently if Varnish is not running. Your Pak does not need to check whether Varnish is installed — just write to the FIFO and move on.

```sh
# This is safe even if Varnish isn't installed
echo "PILL mypak bottom-center 5 Hello" > /tmp/varnish.fifo 2>/dev/null || true
```

---

## Installation

Varnish installs itself when launched for the first time. It:

1. Writes a boot hook to `$USERDATA_PATH/.hooks/boot.d/varnish.sh` so the daemon starts at every boot
2. Wraps `nextui.elf` so `varnish_overlay.so` is preloaded into NextUI's process

**Uninstall** via the `--uninstall` flag or by launching Varnish and selecting uninstall — this cleanly restores `nextui.elf` to its original state.

---

## Building

Requires Docker for cross-compilation.

```sh
# macOS development build
make mac

# Cross-compile for device
make tg5040
make tg5050
make my355

# Build + package all platforms
make package

# Build + push to connected device via ADB
make deploy
```

Produces two artifacts per platform:
- `varnish` — the daemon binary
- `varnish_overlay.so` — the preload hook loaded into nextui.elf
