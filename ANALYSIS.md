# Project Analysis: Stability, Memory Management, and a Possible Rust Rewrite

Written 2026-08-09, prompted by the entity-registry buffer overflow observed on-device
(315 KB `config/entity_registry/list_for_display` response vs the previous 256 KB
`HASS_MAX_JSON_BUFFER`, which silently stalled discovery).

## 1. How big is the project

Hand-written firmware: **~8,950 lines of C++** across 24 files (excluding
`src/assets/` generated icon/font headers, ~740 lines, and the fetched FastEPD /
bb_captouch libraries).

| Area | Files | LOC | Notes |
|---|---|---|---|
| HA WebSocket client | `managers/home_assistant.cpp` | 2,122 | discovery, subscriptions, commands, standby data |
| UI rendering | `managers/ui.cpp` | 1,591 | all screens, partial/full refresh logic |
| Shared store | `store.cpp/.h` | 1,821 | mutex-protected state + snapshots |
| Touch handling | `managers/touch.cpp` | 884 | gesture interpretation per UI mode |
| Wi-Fi | `managers/wifi.cpp` | 580 | profiles, NVS, recovery |
| Widgets | `widgets/*` | ~1,120 | Slider, OnOffButton, Climate, Cover |
| Everything else | main, screen, draw, config, constants | ~830 | |

This is a small codebase by rewrite standards — but the two largest files
(HA client and UI) are also the two hardest parts to reproduce in another language.

## 2. The buffer issue and other likely crash/stall sources

### What actually happened

The failure observed was **not memory unsafety**. The oversized-payload guard in
`hass_ws_event_handler` worked correctly: it dropped the message. The bug is that
**discovery has no timeout or retry** — after the drop, the state machine waits
forever for a reply that was discarded. The device hangs silently rather than
crashing. Bumping the buffer to 512 KB (done, verified on-device) fixes this
instance, but the registry grows with the HA installation, so the cap is a
policy problem, not a sizing problem.

### Audit of related risks (from reading the code)

Hygiene is already fairly good: **zero** uses of `strcpy`/`strcat`/`sprintf`;
50 uses of `snprintf`/`strncpy`; all store data statically allocated with
`MAX_*` caps in `constants.h`; JSON buffer in PSRAM with heap fallback.

Remaining concrete risks, roughly in priority order:

1. **No discovery timeout/retry** (the real cause of the hang). Any dropped or
   unparseable registry response stalls the device until reboot. ~0.5–1 day.
2. **cJSON parse tree of a 300–500 KB document** allocates via `malloc` — whether
   that lands in PSRAM depends on `CONFIG_SPIRAM_USE_MALLOC` thresholds in the
   Arduino build. If it lands in internal RAM, a large registry can OOM and
   cJSON returns null (handled) — but other allocations elsewhere may then fail
   less gracefully. Worth measuring heap watermarks. ~0.5 day to instrument.
3. **Known FIXME in `constants.h`**: no authoritative-vs-target value separation,
   so server updates during the ignore window are lost (UI shows stale values —
   a correctness bug, not a crash). ~1–2 days.
4. **Task stack sizes** (`ui` 4 KB, `home_assistant` 8 KB, `touch` 4 KB) are tight;
   JSON handling runs on the esp_websocket client task. Stack overflow on ESP-IDF
   aborts with a canary panic. Checking `uxTaskGetStackHighWaterMark` is cheap. ~0.5 day.
5. **Streaming/oversize-proof parsing** (the durable fix for #1's root cause):
   cJSON cannot stream; options are a SAX-style parser (jsmn or hand-rolled) just
   for the two registry responses, or chunked server queries. ~2–4 days, the
   largest single item.

### Effort to fix in place

- **Stop the bleeding** (buffer bump + discovery retry/timeout + heap/stack
  instrumentation): **~2–3 days.**
- **Full hardening pass** (all five items above, plus reconnect-path review and a
  bounds audit of the snapshot copies): **~1.5–2 weeks.**
- **Ongoing cost**: C++ discipline, but the codebase already follows consistent
  conventions (static allocation, snapshots under mutex, snprintf-only), and it is
  small enough that a full review stays feasible.

## 3. Rust rewrite: scope and effort

### Two possible stacks

| | std on ESP-IDF (`esp-idf-hal`/`esp-idf-svc`) | bare-metal (`esp-hal`, no_std) |
|---|---|---|
| Wi-Fi/TLS/WebSocket | ESP-IDF services incl. WS client, NVS, Wi-Fi | `esp-wifi`; no ready WS/TLS client stack — significant extra work |
| Fit for this project | Good — mirrors current architecture (FreeRTOS under the hood) | Poor for a networked appliance today |
| Maintenance reality | Community-maintained, "lagging behind latest ESP-IDF", missing HIL tests ([esp-idf-svc](https://github.com/esp-rs/esp-idf-svc)) | Espressif's officially supported direction ([esp-hal](https://github.com/esp-rs/esp-hal)) |

Note the tension: the stack that fits this project (std/ESP-IDF) is the
community-maintained one; the officially supported one (esp-hal) lacks the
networking stack this project needs.

### The hard parts of a port

1. **Display driver is the biggest risk.** FastEPD is C++ (class-based — not
   bindgen-friendly). Options: bind [epdiy](https://github.com/vroland/epdiy)
   (C, bindable under an ESP-IDF build) and reimplement FastEPD's grayscale/
   partial-refresh/font layer on top, or start from the experimental
   [lilygo-epd47 crate](https://crates.io/crates/lilygo-epd47) (one board variant
   only, "basic functionality, simplified"). M5PaperS3 support would be from
   scratch either way. Also loses the `patch_fastepd_glyph_width` fix and the
   tuned pass counts — all the display quality work gets redone. **2–4 weeks alone.**
2. **HA WebSocket client + discovery** (~2,100 lines): serde makes the JSON far
   nicer, but all the protocol/state-machine logic must be rebuilt. ~1–2 weeks.
3. **UI + touch + widgets** (~3,600 lines): mechanical but large; every layout
   constant and gesture behavior re-verified on hardware. ~2–3 weeks.
4. **Toolchain**: ESP32-S3 is Xtensa, which mainline rustc/LLVM does not support —
   development requires the espup-managed Rust fork
   ([Rust on ESP Book](https://docs.espressif.com/projects/rust/book/introduction/hardware-overview.html)).
   Workable but adds CI and contributor friction. Espressif's newer chips are
   RISC-V (mainline Rust), so Xtensa is the legacy path.

**Total estimate: 6–10 weeks to feature parity for one board**, with the display
driver as the least predictable item. Roughly **5× the full C++ hardening pass**.

### How much would Rust actually solve?

What it eliminates: out-of-bounds writes, use-after-free, data races
(Send/Sync enforced at compile time — genuinely valuable for this
four-task-shared-store design). Failure mode improves: a bug panics with a
backtrace instead of corrupting memory.

What it does **not** solve — and this matters here: **both issues found on-device
today were logic bugs.** A fixed-capacity buffer policy and a missing retry would
behave identically in Rust (`Vec::with_capacity` + a length check + no timeout =
the same silent stall). OOM on a 16 MB-flash/8 MB-PSRAM device is still possible.
The epdiy binding would remain an `unsafe` FFI boundary. Rust raises the floor on
a class of bugs this codebase has so far avoided anyway (no unsafe string calls,
static allocation), rather than fixing the class it actually has.

## 4. PlatformIO and Rust

- **PlatformIO has no native Rust support.** The request is an open issue from 2019
  ([platformio-core#2947](https://github.com/platformio/platformio-core/issues/2947))
  with no roadmap commitment.
- **[cargo-pio](https://crates.io/crates/cargo-pio)** (part of the esp-rs
  [embuild](https://github.com/esp-rs/embuild) project) bridges the two: either
  Cargo-first (PIO used internally to build/link the vendor SDK) or PIO-first
  (a PIO project that calls into a Rust staticlib). As of Aug 2026, cargo-pio's
  latest release (0.26.0) is **over a year old**; the parent embuild crate saw a
  release ~9 months ago. It works, but it is niche and thinly maintained.
- **Practical reality**: the esp-rs community has converged on plain
  `cargo` + `espflash` + `espup`, not PlatformIO. A Rust port would effectively
  mean leaving PlatformIO (and the current CI workflow) rather than integrating
  with it.

## 5. Bottom line

| Option | Effort | Residual risk |
|---|---|---|
| Targeted fixes (retry + instrumentation) | 2–3 days | large-registry parsing still cap-bound |
| Full C++ hardening pass | 1.5–2 weeks | C++ discipline required ongoing |
| Rust rewrite (std/ESP-IDF) | 6–10 weeks | logic bugs unaffected; display driver risk; fork toolchain; thinner ecosystem |

**Recommendation: harden in place.** The codebase is small, its memory hygiene is
already better than average, and the observed failures are protocol/logic-level —
the category Rust does not fix. The rewrite's one genuinely strong argument
(compile-time data-race safety across the four tasks) doesn't outweigh redoing
the display layer against an ecosystem with no mature parallel-e-ink driver.
Revisit Rust if the project ever moves to a RISC-V ESP32 (mainline toolchain) or
if epdiy grows first-class Rust bindings.

### Sources

- [esp-rs/esp-hal](https://github.com/esp-rs/esp-hal)
- [esp-rs/esp-idf-svc](https://github.com/esp-rs/esp-idf-svc)
- [esp-hal 1.0 beta announcement](https://developer.espressif.com/blog/2025/02/rust-esp-hal-beta/)
- [The Rust on ESP Book — Hardware Overview](https://docs.espressif.com/projects/rust/book/introduction/hardware-overview.html)
- [platformio-core issue #2947 — Add support for Rust](https://github.com/platformio/platformio-core/issues/2947)
- [cargo-pio on crates.io](https://crates.io/crates/cargo-pio)
- [esp-rs/embuild](https://github.com/esp-rs/embuild)
- [lilygo-epd47 crate](https://crates.io/crates/lilygo-epd47)
- [vroland/epdiy](https://github.com/vroland/epdiy)
- [bitbank2/FastEPD](https://github.com/bitbank2/FastEPD)
