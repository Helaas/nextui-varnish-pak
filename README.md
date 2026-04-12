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

If you already know the daemon is running, you can write a single line to the FIFO:

```sh
echo "PILL mypak bottom-center 5 Scraping artwork..." > /tmp/varnish.fifo
```

To hide it before it expires:

```sh
echo "HIDE mypak" > /tmp/varnish.fifo
```

For normal Pak integration, source the helper script bundled with Varnish. It prefers the
bundled `varnish` binary so FIFO writes stay non-blocking:

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

These direct FIFO examples assume the Varnish daemon is already running:

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

The C helper in `include/varnish.h` and the bundled `scripts/varnish.sh` helper both treat
Varnish IPC as best-effort and avoid blocking when the daemon is down.

If you choose to write to `/tmp/varnish.fifo` yourself from shell, do not assume a plain redirect
is always safe. A FIFO can block if the file exists without an active reader, such as after a
daemon crash. Guard direct writes with the PID file first:

```sh
# Best-effort send: only write if the Varnish daemon is running
if [ -r /tmp/varnish.pid ] && kill -0 "$(cat /tmp/varnish.pid)" 2>/dev/null; then
  printf '%s\n' "PILL mypak bottom-center 5 Hello" > /tmp/varnish.fifo 2>/dev/null || true
fi
```

---

## Managing Varnish

Launching `Varnish.pak` now opens a small management UI instead of auto-installing and auto-starting the daemon.

The main screen exposes:

- `Enabled` — install/remove the startup wiring and start/stop the daemon
- `Hotkeys` — configure global button chords handled by the daemon

The live status still shows:

- `Startup patch: Installed/Missing`
- `Boot hook: Installed/Missing`
- `Daemon: Running/Stopped`

Hotkeys are stored at:

```
~/.userdata/<platform>/Varnish/keybinds.txt
```

Current v1 action:

- `Screenshot` — capture the current framebuffer to `/mnt/SDCARD/Screenshots`

Hotkeys are active only while Varnish is enabled and its daemon is running. The settings UI pauses hotkeys while it is open so editing a binding does not accidentally trigger it.

Saving `Enabled = On`:

1. Writes `~/.userdata/<platform>/Varnish/enabled`
2. Patches `.tmp_update/<platform>.sh` so the NextUI boot chain exports `LD_PRELOAD`
3. Writes `~/.userdata/<platform>/.hooks/boot.d/varnish.sync.sh`
4. Starts the daemon immediately if it is not already running

Saving `Enabled = Off`:

1. Removes the enabled marker
2. Removes the startup patch
3. Removes the boot hook
4. Stops the running daemon immediately

Both operations require a reboot for the current launcher session to fully pick up the change.

---

## CLI modes

| Command | Description |
|---|---|
| `varnish --ui` | Open the management UI |
| `varnish --daemon` | Start the daemon directly |
| `varnish --install` | Enable startup wiring and start the daemon |
| `varnish --pill <client_id> <position> <duration_secs> <text>` | Send a pill command with a non-blocking FIFO open |
| `varnish --hide <client_id>` | Hide a client's pill with a non-blocking FIFO open |
| `varnish --clear` | Clear all pills with a non-blocking FIFO open |
| `varnish --kill` | Stop the running daemon |
| `varnish --uninstall` | Disable startup wiring and stop the daemon |

Running `varnish` with no arguments behaves the same as `varnish --install`.

---

## Developer Notes

Varnish targets the `NextUI_old` hook model and treats `NextUI_old/releases/NextUI-20260325-hooks-0-all.zip` as the TrimUI source of truth for launcher behavior.

`boot.d` is not the `LD_PRELOAD` injection point. `NextUI_old` runs boot hooks through `run_hooks.sh`, and each hook runs in its own subshell. That means a boot hook can repair files and start the daemon, but it cannot export `LD_PRELOAD` back into the parent launcher shell before `nextui.elf` or Pak launches.

For that reason Varnish patches `.tmp_update/<platform>.sh` instead:

- The patched platform script calls `varnish --startup-env` just before it launches `MinUI.pak/launch.sh`.
- `varnish --startup-env` re-applies the startup patch if the user is enabled, then prints shell code that exports `LD_PRELOAD` for the rest of that boot chain.
- This reaches both the main NextUI process and later Pak launches, which is the part the old `nextui.elf` wrapper could not cover.

The boot hook exists for repair and daemon health:

- `~/.userdata/<platform>/.hooks/boot.d/varnish.sync.sh` runs synchronously.
- It checks the enabled marker and startup patch.
- If the user is enabled but the startup patch is missing, it re-applies the patch and forces a reboot so the next boot runs through the repaired launcher path.
- If the user is enabled and the daemon is down, it starts `varnish --daemon`.

Varnish intentionally does not patch the common `.tmp_update/updater` or NextUI `launch.sh`. The updater only selects a platform script, and the launcher is too late to make `LD_PRELOAD` survive card updates without broader patching.

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
- `varnish_overlay.so` — the preload hook injected through the patched NextUI startup chain
