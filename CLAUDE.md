# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

ESP32-S3 (Arduino framework) firmware for an e-ink touchscreen Home Assistant remote, built with PlatformIO and the FastEPD library. It talks to Home Assistant directly over the WebSocket API (long-lived access token, no server-side plugin). `REQUIREMENTS.md` is the product spec — consult it when changing UI behavior or navigation.

## Commands

Two PlatformIO environments (pick per target hardware):

- `lilygo-t5-s3` — Lilygo T5 E-Paper S3 Pro
- `m5-papers3` — M5Stack M5Paper S3

```bash
pio run -e lilygo-t5-s3            # build
pio run -e lilygo-t5-s3 -t upload  # flash
pio run -e lilygo-t5-s3 -t monitor # serial monitor (close before flashing again)
```

CI (`.github/workflows/platformio-build.yml`) builds both environments.

E2E tests (`e2e/`, pytest) drive the real device over its built-in HTTP harness and verify against the live Home Assistant instance:

```bash
uv run pytest -m "not slow"   # needs the device flashed + .env (see e2e/config.py)
```

The harness (`src/managers/harness.cpp`, always compiled in, port 8080) exposes `/health`, `/state`, `/tap`, `/swipe`, `/home`, and `/screenshot` (raw framebuffer; decoded to PNG by `e2e/device.py`). Touch injection enters through `harness_get_samples()` — the only `getSamples` call site in `touch.cpp` — so synthetic gestures exercise the full gesture pipeline. `e2e/layout.py` ports the tile-grid math from `touch.cpp`; keep them in sync when layout constants change. Tests actuate real office devices (light, blinds, AC) configured in `.env`.

Building requires `src/config_remote.cpp` (gitignored) — copy from `src/config_remote.cpp.example` and fill in Wi-Fi/HA credentials.

Icon regeneration: drop 256x256 MDI PNGs in `icons-buttons/` (need matching `-on`/`-off` pairs) or `icons-ui/`, then run `generate-icons.py` (Python with Pillow; project uses uv — `uv run generate-icons.py`). It emits the gitignored `src/assets/icons.h`.

Formatting: `.clang-format` at the repo root (LLVM base, 4-space indent, 135 column limit).

## Architecture

Three FreeRTOS tasks plus the Arduino `loop()`, all created in `src/main.cpp`, communicating only through a mutex-protected shared store:

- **`ui_task`** (`src/managers/ui.cpp`) — owns the FastEPD display. Blocks on a task notification, then diffs the desired `UIState` (computed from the store via `store_update_ui_state`) against what's on screen and redraws. Renders every `UiMode` (boot/error screens, floor list, room list, room controls, settings, Wi-Fi, standby).
- **`touch_task`** (`src/managers/touch.cpp`) — polls the GT911 touch controller (bb_captouch), interprets taps/swipes against the published UI state, and mutates the store (navigation, commands, keyboard input).
- **`home_assistant_task`** (`src/managers/home_assistant.cpp`) — WebSocket client: authenticates, discovers floors/areas/entities from the HA registries, subscribes to state changes, fetches standby weather/energy data, and sends queued commands.
- **`loop()`** — Wi-Fi polling/recovery (`src/managers/wifi.cpp`, profiles persisted in NVS) and the hardware home button.

### The store is the hub (`src/store.cpp` / `store.h`)

`EntityStore` holds all shared state: connection states, floors/rooms/entities, navigation position, Wi-Fi settings state, standby data. Rules that keep this thread-safe:

- All mutation goes through `store_*` functions that take the mutex; writers call `notify_ui()` (a task notification to `ui_task`) to trigger a redraw.
- Readers (mainly the UI task) copy data out under the lock via `*Snapshot` structs (`FloorListSnapshot`, `RoomControlsSnapshot`, `WifiSettingsSnapshot`, `StandbySnapshot`, …) — never render while holding the mutex.
- Revision counters (`rooms_revision`, `settings_revision`, `standby_revision`) in the store are compared against `UIState` to decide when a screen is stale.
- Commands flow one way: touch → `store_send_command` marks an entity's command pending and notifies `home_assistant_task`, which drains them via `store_get_pending_command` / `store_ack_pending_command`.

`SharedUIState` (`src/ui_state.cpp`) is a separate versioned, mutex-guarded copy of the current `UIState`, published by the UI task so the touch task knows what's on screen.

Everything is statically allocated with fixed-size arrays and char buffers; all `MAX_*` limits and tuning constants (command throttling, standby timeouts, layout dimensions) live in `src/constants.h`.

### Rendering

Widgets (`src/widgets/`) implement the `Widget` interface: `fullDraw`, `partialDraw` (returns the dirty rect), `isTouching`, `getValueFromTouch`. `Screen` holds the widget list for the current room-controls page, built in the store when a room is selected. Drawing uses 1bpp for fast partial updates and 4bpp (grayscale) for full refreshes — widgets receive the `BitDepth` and must handle both.

### Board support

Board differences (pins, display panel, home button) are isolated in `src/boards.h`, selected by `-DTARGET_*` build flags from `platformio.ini`. Keep board-specific code behind those constants rather than scattering `#ifdef`s.

### FastEPD patch

`tools/patch_fastepd_glyph_width.py` runs as a PlatformIO pre-script and patches a glyph-width guard inside the fetched FastEPD library. If text rendering breaks after a FastEPD update, check whether this patch still applies.
