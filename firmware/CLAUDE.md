# MacroKeyboard — CLAUDE.md (`macro_keyboard`)

> **IMPORTANT:** Use the *latest* official ZMK documentation as the source of truth.
> Docs root: <https://zmk.dev/docs> (prefer the "development" docs when there is a mismatch).
> If uncertain about a flag/snippet/option name, look it up in the current ZMK docs before changing code.

> **This file describes the project as it is now.** The milestone-by-milestone
> build history and the original (baseline) functional requirements live in
> [IMPLEMENTATION_PLAN.md](./IMPLEMENTATION_PLAN.md). When the two disagree,
> this file wins. All milestones M1–M6 are complete (M6 has only user
> hardware-verification items remaining — see the plan file).

## Project Context

- **Device:** "macro_keyboard" (custom BOARD, not a shield)
- **MCU:** nRF52840 module MDBT50Q-1MV2 (Bluetooth LE)
- **Power:** 2000 mAh LiPo (battery-powered)
- **Input:** 12-key matrix (3 rows x 4 cols), plus a dedicated "profile switch" button
- **Output/UI:** OLED 128x64 over I2C; shows battery, connection type, encoder mode, and the profile column with the active profile highlighted
- **Rotary/Encoder:** AS5600 (I2C) magnetic encoder, per-profile configurable mode (volume / vertical scroll / horizontal scroll / browser tabs)
- **Firmware:** ZMK, with key macros configurable through ZMK Studio (encoder modes are firmware-owned — Studio can't edit sensor bindings in this ZMK version)
- **Host bias:** dev/test on macOS — default keymap shortcuts use Cmd (LG); user can rebind in Studio.

## Functional Requirements — Current Behaviour

*(For the original baseline requirements, see [IMPLEMENTATION_PLAN.md](./IMPLEMENTATION_PLAN.md). The main evolution: the fixed "4 profiles" became a flexible 1–7, and the encoder gained a per-profile mode with a browser-tabs action.)*

1. **Connectivity**
   - Acts as a BLE HID keyboard/media/pointing device; works OS-agnostically (Windows/macOS/Linux/iOS/Android) via standard HID. USB HID also supported.
   - Up to 5 paired BLE hosts (`BT_MAX_PAIRED=5`, `BT_MAX_CONN=5`). Host-switch keys (`&bt`, `&out`) are not pre-bound — assign in Studio.
2. **Profiles (flexible, 1–7)**
   - Ships with 5 profiles active; a single hardware button (pos 12, `&profile_next`) cycles through the *live* valid profiles and wraps.
   - Profiles can be added / removed / renamed in ZMK Studio (2 reserved spare slots → up to 7, the screen's `MAX_PROFILES`); changes persist in NVS. Profile-count changes are tracked live by the cycle button, encoder-mode state, and the display.
   - Each profile defines:
     - Key macros (media keys, shortcuts, app-launch sequences where supported by host OS) — editable in Studio.
     - An encoder mode (volume / vertical scroll / horizontal scroll / browser tabs), independent per profile.
3. **Encoder**
   - AS5600 magnetic encoder; the active profile's mode decides the action each detent. `&enc_mode_next` (pos 3) cycles the current profile's mode; the mode persists in NVS.
   - CW = volume-up / scroll-up / scroll-right / next-tab. Scroll feel is host-owned (firmware emits a clean linear stream — see gotchas).
4. **Display**
   - Always shows: battery indicator (icon + %, with a charging bolt on USB), connection type (USB/BLE), current encoder mode, and a profile column with the active profile highlighted; the column scales to the live profile count.
   - Updates on profile change, encoder-mode change, connection change, and periodically for battery status.
5. **Macros / Keymap editing**
   - Key bindings and profiles are editable via ZMK Studio (RPC over USB in the studio variant). Encoder/sensor bindings are firmware-owned (Studio's keymap proto is key-position only in this ZMK version).
6. **Power**
   - Battery ADC reading is exposed to firmware and shown on the UI.
   - Low-power idle/deep-sleep per ZMK defaults; the AS5600 poll thread activity-gates its rate (5 ms → 100 ms when idle) so the still magnet doesn't block SoC idle.

## Pinmap

*(from Macro-Keyboard-v4 schematic)*

- **Matrix columns:** col0=P0.16, col1=P0.14, col2=P0.15, col3=P0.13
- **Matrix rows:** row0=P0.20, row1=P0.21, row2=P0.19
- **I2C bus:** SDA=P0.24, SCL=P0.25 (OLED + AS5600 on same bus)
- **Profile button:** PROFILE=P1.02 (wired P1.02 → diode anode, cathode → GND; pressed line settles at ~0.7V — kscan uses polling)
- **Battery ADC:** VD_ADC=P0.04
- (AS5600 OUT pin exists on PCB: ENC_OUT=P0.29; use only if needed in future revisions)

## Repo Layout

- **Workspace:** `/firmware/zmk_toolchain/{app,modules,zephyr}`
- **User config:** `/firmware/zmk-config/config/`
- **Board files:** `/firmware/zmk-config/config/boards/arm/macro_keyboard/`
- **Custom firmware module** (pulled in via `-DZMK_EXTRA_MODULES`): `/firmware/zmk-config/module/`
  - `zephyr/module.yml` — Zephyr-module declaration (cmake + dts_root)
  - `CMakeLists.txt` — conditionally compiles each piece
  - `dts/bindings/behaviors/...` — DT bindings for custom behaviors
  - `src/behavior_profile_next.c` — custom `&profile_next` ZMK behavior (M3)
  - `src/status_screen.c` — custom status screen — added in M4
  - `drivers/sensor/as5600/...` — AS5600 encoder driver (M5)
  - `src/encoder_mode.c` — per-profile encoder-mode state (M5)
  - `src/behavior_encoder_dispatch.c` — `&enc_dispatch` sensor behavior (M5)
  - `src/behavior_encoder_mode_next.c` — `&enc_mode_next` key behavior (M5)
  - `src/fatal.c` — reboot-on-fatal-error handler (M6 resilience)
- **Bootloader** (UF2, default flash workflow — see [Bootloader](#bootloader)): `/firmware/bootloader/macro_keyboard_bootloader.hex`

## Local Builds — Always

All build commands must be run from `/firmware/zmk_toolchain/app`.

Always activate the venv first:

```bash
source /firmware/zmk_toolchain/.venv/bin/activate
```

Use a distinct build directory per build: `build/<board>/<variant>/`

All builds need BOTH the user config AND the custom-firmware module (the module supplies the `&profile_next` behavior, the custom status screen, the AS5600 driver, the encoder behaviors, and the fatal-error handler).

**Base build (pristine, dev variant):**

```bash
west build -p -d build/macro_keyboard/m3 -b macro_keyboard -- \
  -DZMK_CONFIG="/Users/nikolaskelava/Documents/macro_keyboard/firmware/zmk-config/config" \
  -DZMK_EXTRA_MODULES="/Users/nikolaskelava/Documents/macro_keyboard/firmware/zmk-config/module"
```

**Incremental rebuild (no flags needed — they are cached):**

```bash
west build -d build/macro_keyboard/m3
```

**ZMK Studio build (adds RPC transport snippet):**

```bash
west build -p -d build/macro_keyboard/m3_studio -b macro_keyboard -S studio-rpc-usb-uart -- \
  -DZMK_CONFIG="/Users/nikolaskelava/Documents/macro_keyboard/firmware/zmk-config/config" \
  -DZMK_EXTRA_MODULES="/Users/nikolaskelava/Documents/macro_keyboard/firmware/zmk-config/module" \
  -DCONFIG_ZMK_STUDIO=y
```

## Flashing

Default workflow is UF2 drag-drop (Adafruit nRF52 bootloader installed). The build produces both `zmk.uf2` (drag-drop) and `zmk.hex` (SWD).

**UF2 (default — fast iteration):**

1. Double-tap the reset signal on the board. The device mounts as a USB MSC drive (~"NRF52BOOT").
2. Drag-drop `build/macro_keyboard/m3/zephyr/zmk.uf2` onto that drive. The bootloader writes the app, ejects, and resets into firmware.

**SWD via pyocd** (only when the bootloader is misbehaving, or for first bootloader install / re-install):

```bash
west flash -d build/macro_keyboard/m3 -r pyocd
```

pyocd will flash `zmk.hex` directly to 0x26000 — bootloader area (0x0..0x25FFF, 0xF4000..0x100000) is left untouched.

## Bootloader

A UF2 bootloader (Adafruit_nRF52_Bootloader, `nice_nano_v2` build — same SoC family as the MDBT50Q-1MV2 module, no external QSPI) is the default flashing target. The binary lives at `firmware/bootloader/macro_keyboard_bootloader.hex`.

The build is wired for it by default — see `macro_keyboard_defconfig`:

```
CONFIG_USE_DT_CODE_PARTITION=y       (links app at code_partition = 0x26000)
CONFIG_BUILD_OUTPUT_UF2=y             (emits zmk.uf2)
CONFIG_BUILD_OUTPUT_UF2_FAMILY_ID="0xADA52840"   (Adafruit nRF52840 family ID)
```

**DTS partition layout** (in `macro_keyboard.dts`, `&flash0`):

```
0x000000 – 0x025FFF  reserved by bootloader (MBR + bootloader header)
0x026000 – 0x0EBFFF  code_partition     (~792 KB)
0x0EC000 – 0x0F3FFF  storage_partition  (32 KB NVS)
0x0F4000 – 0x0FFFFF  reserved for bootloader code
```

**Bootloader install / re-install** (one-time, via SWD/DAPLink):

```bash
pyocd flash --target=nrf52840 \
  /Users/nikolaskelava/Documents/macro_keyboard/firmware/bootloader/macro_keyboard_bootloader.hex
```

After this, double-tapping reset mounts the USB MSC drive.

**If the bootloader has to be removed** (full chip erase, no UF2): Revert `macro_keyboard_defconfig` (drop `USE_DT_CODE_PARTITION` + the two `BUILD_OUTPUT_UF2` lines), revert the DTS partition layout to start at 0x000000 with size 0xF0000, pristine-rebuild, and flash via SWD. The bootloader is then gone — drag-drop UF2 will not work until reinstalled.

> **DANGER:** `USE_DT_CODE_PARTITION=y` means the firmware's reset vector lives at 0x26000, NOT 0x0. If the bootloader is ever erased without also reverting this flag, the CPU resets into an empty vector table at 0x0 and the board appears bricked (SWD still works to recover; either reinstall the bootloader or revert the flag and reflash).

## Load-Bearing Constraints & Gotchas

These are the things that silently brick, hang, or corrupt the board if changed
without care. Each points to the milestone in
[IMPLEMENTATION_PLAN.md](./IMPLEMENTATION_PLAN.md) with the full story.

**Build both variants.** Any Kconfig/DTS change must be rebuilt and checked in
*both* the dev variant and the `studio-rpc-usb-uart` variant before it's "done".

**Bootloader reset vector** — see the [DANGER note](#bootloader): with
`USE_DT_CODE_PARTITION=y` the reset vector is at 0x26000; erasing the bootloader
without reverting that flag appears to brick the board.

**Display — six Kconfig pins (plan → M4).** All pinned in board
`Kconfig.defconfig` under `if LVGL` / `if ZMK_DISPLAY`. Removing ANY silently
hangs at boot and BLE/USB never enumerate:
1. `LV_COLOR_DEPTH_1` + `LV_Z_BITS_PER_PIXEL=1` (match the 1bpp SSD1306).
2. `LV_Z_VDB_SIZE=64` (default 10% starves the I2C-served display work queue).
3. `ZMK_DISPLAY_WORK_QUEUE_DEDICATED` (isolates LVGL stalls from BLE/USB TX).
4. `LV_USE_THEME_MONO=y` (CUSTOM screen doesn't auto-imply it → NULL font → hardfault).
5. `LV_FONT_MONTSERRAT_16` + `LV_FONT_DEFAULT_MONTSERRAT_16` (same CUSTOM gap).
6. `LV_Z_MEM_POOL_SIZE=12288` (CUSTOM falls back to Zephyr's 2048 → alloc NULL → hardfault; 12288 has headroom for the battery-icon rework).

Also required for the custom screen: `LV_FONT_UNSCII_8=y`, `LV_USE_CANVAS=y`.

**Display — 1bpp LVGL canvas rule (plan → M6 USB crash).** On a
`LV_COLOR_DEPTH_1` build, NEVER make an `lv_canvas`/image a clipped child of a
styled/bordered container, and never let it overlap another object — that
triggers an RGB565 intermediate-layer path whose buffer math is wrong for 1bpp
and blends out of bounds → MPU fault (this is what caused the USB-only crash via
the charging bolt). Keep decorative canvases as standalone, unclipped, direct
children of the screen. And every I1 canvas static buffer must add
`I1_PALETTE_BYTES` (= `LV_COLOR_INDEXED_PALETTE_SIZE(I1)*4` = 8 B) on top of
`LV_CANVAS_BUF_SIZE`, taking the bitmap base from `lv_draw_buf_goto_xy(buf,0,0)`.

**Encoder — AS5600 driver invariants (plan → M5/M6).**
- The driver POLLS on a *dedicated thread* (kept off the system work queue so a blocked I2C read on the OLED-shared bus can't stall BLE/USB). Custom compatible `nikolas,as5600`, NOT `ams,as5600` (would double-instantiate).
- `AS5600_DEADBAND_COUNTS` (4) is LOAD-BEARING: without it a still magnet's ±1-2 LSB jitter random-walks past the 1° report threshold, raising spurious sensor events that reset the idle timer (board never sleeps) and eventually hang USB.
- `AS5600_STARTUP_DELAY_MS` (1000) lets OLED/BLE/USB init before the first shared-bus read (otherwise intermittent boot hang).
- Sensor report needs val1 ≥ 1 (≥1° accumulated); a val1==0 report hits a legacy raw-tick path in ZMK's sensor-rotate behaviour.
- Idle activity-gating backs the poll period off 5 ms → 100 ms when not `ZMK_ACTIVITY_ACTIVE`, so the still magnet doesn't block SoC low-power idle.

**Encoder — scroll must be coalesced (plan → M5 USB hang).** The
sensor→behaviour→HID chain runs synchronously on the poll thread and each USB
HID send blocks on a 30 ms semaphore; one report per ~200 Hz tick wedges USB HID
TX and hangs the board. `&enc_dispatch` coalesces scroll to at most one report
per `scroll-min-interval-ms` (12), carrying the signed remainder. Scroll feel is
HOST-OWNED — firmware emits a clean linear line-delta stream (no firmware accel);
a host app (Mac Mouse Fix) owns smoothing/accel. Don't re-litigate.

**Encoder modes are firmware-owned.** ZMK Studio can't edit encoder/sensor
bindings in this ZMK version (keymap proto is key-position only), so per-profile
modes live in `encoder_mode.c` and persist to NVS (`encmode/modes`, saved
immediately, not on the 60 s debounce).

**Profile count is live.** `&profile_next` and `encoder_mode.c` size/wrap on the
*live* valid-layer count so Studio add/remove/rename tracks correctly; the screen
scales to it. `MAX_PROFILES` is 7.

**Resilience (permanent).** `CONFIG_HW_STACK_PROTECTION=y` (MPU guard) and
`module/src/fatal.c` override Zephyr's spin-forever `k_sys_fatal_error_handler`
with `sys_reboot()` (needs `CONFIG_REBOOT=y`) so a fault recovers instead of
hanging. If a fault recurs, the no-debugger on-screen fault-capture recipe is in
the plan file (M6).

## Toolchain

Native Zephyr/ZMK install on M1 Mac (followed the ZMK "Getting Started" native setup guide). Toolchain at `/firmware/zmk_toolchain/{app,modules,zephyr}`.

Always: `source /firmware/zmk_toolchain/.venv/bin/activate` before building.

## Board Files — Current State

Vendor/name: nikolas / macro_keyboard (HWMv2, SoC nrf52840)

Files under `zmk-config/config/boards/arm/macro_keyboard/`:

- board.yml, board.cmake, pre_dt_board.cmake
- Kconfig.macro_keyboard, Kconfig.defconfig
- macro_keyboard.yaml, macro_keyboard_defconfig
- macro_keyboard.dts

Keymap + config (user-side, picked up via ZMK_CONFIG):

- `zmk-config/config/macro_keyboard.keymap` (5 blank profiles + 2 reserved spares; pos 3 `&enc_mode_next`, pos 12 `&profile_next`)
- `zmk-config/config/macro_keyboard.conf`

DTS wiring (matches [Pinmap](#pinmap)):

- **kscan_matrix** — zmk,kscan-gpio-matrix, col2row, rows P0.20/21/19, cols P0.16/14/15/13
- **kscan_direct** — zmk,kscan-gpio-direct, P1.02 (profile button)
- **kscan_composite** — combines both into a single 4r x 4c kscan; `zmk,kscan = &kscan_composite` (chosen)
- **matrix xform** — matrix_transform0 (4r x 4c, sparse — 13 entries)
- **phys layout** — physical_layout0 (13 keys, profile btn at (300, 0))
- **i2c0** — P0.24 SDA / P0.25 SCL, fast mode; oled@0x3c (CUSTOM status screen, M4) + as5600@0x36 (nikolas,as5600, M5)
- **sensors** — zmk,keymap-sensors node → &as5600 (M5; resolution is per-mode on &enc_dispatch, not on this node)
- **adc** — enabled; vbatt on AIN2 (P0.04) via zmk,battery-voltage-divider (330k Batt+→VD_ADC, 1M VD_ADC→GND → `output-ohms=1000000`, `full-ohms=1330000`, ×1.33; on-screen % still to be calibrated against a multimeter)

## Current Flash Partition Layout — UF2 Bootloader Installed

```
0x000000 – 0x025FFF  reserved by bootloader (MBR + bootloader header)
0x026000 – 0x0EBFFF  code_partition     (~792 KB ZMK app — links here)
0x0EC000 – 0x0F3FFF  storage_partition  (32 KB NVS)
0x0F4000 – 0x0FFFFF  reserved for bootloader code
```

See [Bootloader](#bootloader) above for the install procedure and the (rare) revert path back to direct-SWD.
