# MacroKeyboard — CLAUDE.md (macro_keyboard)
# IMPORTANT: Use the *latest* official ZMK documentation as the source of truth.
# Docs root: https://zmk.dev/docs (prefer the "development" docs when there is a mismatch).
# If uncertain about a flag/snippet/option name, look it up in the current ZMK docs before changing code.

[PROJECT CONTEXT]
Device: "macro_keyboard" (custom BOARD, not a shield)
MCU: nRF52840 module MDBT50Q-1MV2 (Bluetooth LE)
Power: 2000 mAh LiPo (battery-powered)
Input: 12-key matrix (3 rows x 4 cols), plus a dedicated "profile switch" button
Output/UI: OLED 128x64 over I2C; shows battery state + profiles (1–4) with active profile highlighted
Rotary/Encoder: AS5600 (I2C) used as a configurable control (e.g., volume, vertical scroll, horizontal scroll)
Firmware: ZMK, with macros configurable through ZMK Studio
Host bias: dev/test on macOS — default keymap shortcuts use Cmd (LG); user can rebind in Studio.

[FUNCTIONAL REQUIREMENTS — BEHAVIORAL OVERVIEW]
1) Connectivity
   - Acts as a BLE HID keyboard/media device; should work OS-agnostically (Windows/macOS/Linux/iOS/Android) via standard HID.
2) Profiles (4 total)
   - A single hardware button cycles active profile 1→2→3→4→1.
   - Each profile defines:
     a) Key macros (media keys, shortcuts, app-launch sequences where supported by host OS)
     b) Encoder behavior (volume or scroll; direction/axis can differ per profile)
3) Display
   - Always shows:
     - Battery indicator (percentage/level derived from ADC measurement)
     - Profile list 1–4, with active profile visually marked
   - Display updates on profile change and periodically for battery status.
4) Macros / Keymap editing
   - All macros and key bindings must be editable via ZMK Studio.
   - Ensure the configuration includes whatever is required by the current ZMK Studio workflow (per latest ZMK docs).
5) Power
   - Battery ADC reading is exposed to the firmware (and optionally to UI).
   - Aim for low-power behavior consistent with ZMK defaults (sleep/idle as supported).

[PINMAP] (from Macro-Keyboard-v4 schematic)
Matrix columns: col0=P0.16, col1=P0.14, col2=P0.15, col3=P0.13
Matrix rows:    row0=P0.20, row1=P0.21, row2=P0.19
I2C bus:        SDA=P0.24, SCL=P0.25  (OLED + AS5600 on same bus)
Profile button: PROFILE=P1.02  (wired P1.02 → diode anode, cathode → GND;
                pressed line settles at ~0.7V — kscan uses polling)
Battery ADC:    VD_ADC=P0.04
(AS5600 OUT pin exists on PCB: ENC_OUT=P0.29; use only if needed in future revisions)

[REPO LAYOUT]
Workspace:
  /firmware/zmk_toolchain/{app,modules,zephyr}
User config:
  /firmware/zmk-config/config/
Board files:
  /firmware/zmk-config/config/boards/arm/macro_keyboard/
Custom firmware module (pulled in via -DZMK_EXTRA_MODULES):
  /firmware/zmk-config/module/
  - zephyr/module.yml             : Zephyr-module declaration (cmake + dts_root)
  - CMakeLists.txt                : conditionally compiles each piece
  - dts/bindings/behaviors/...    : DT bindings for custom behaviors
  - src/behavior_profile_next.c   : custom &profile_next ZMK behavior (M3)
  - src/status_screen.c           : custom status screen — added in M4
  - drivers/sensor/as5600/...     : AS5600 encoder driver — added in M5
Bootloader (UF2, default flash workflow — see [BOOTLOADER]):
  /firmware/bootloader/macro_keyboard_bootloader.hex

[LOCAL BUILDS — ALWAYS]
All build commands must be run from:
  /firmware/zmk_toolchain/app
Always activate the venv first:
  source /firmware/zmk_toolchain/.venv/bin/activate
Use a distinct build directory per build:
  build/<board>/<variant>/

All builds need BOTH the user config AND the custom-firmware module (the
module supplies the &profile_next behavior, and later the custom status
screen + AS5600 driver).

Base build (pristine, dev variant):
  west build -p -d build/macro_keyboard/m3 -b macro_keyboard -- \
    -DZMK_CONFIG="/Users/nikolaskelava/Documents/macro_keyboard/firmware/zmk-config/config" \
    -DZMK_EXTRA_MODULES="/Users/nikolaskelava/Documents/macro_keyboard/firmware/zmk-config/module"

Incremental rebuild (no flags needed — they are cached):
  west build -d build/macro_keyboard/m3

ZMK Studio build (adds RPC transport snippet):
  west build -p -d build/macro_keyboard/m3_studio -b macro_keyboard -S studio-rpc-usb-uart -- \
    -DZMK_CONFIG="/Users/nikolaskelava/Documents/macro_keyboard/firmware/zmk-config/config" \
    -DZMK_EXTRA_MODULES="/Users/nikolaskelava/Documents/macro_keyboard/firmware/zmk-config/module" \
    -DCONFIG_ZMK_STUDIO=y

[FLASHING]
Default workflow is UF2 drag-drop (Adafruit nRF52 bootloader installed).
The build produces both `zmk.uf2` (drag-drop) and `zmk.hex` (SWD).

  UF2 (default — fast iteration):
    1. Double-tap the reset signal on the board.
       The device mounts as a USB MSC drive (~"NRF52BOOT").
    2. Drag-drop  build/macro_keyboard/m3/zephyr/zmk.uf2  onto that drive.
       The bootloader writes the app, ejects, and resets into firmware.

  SWD via pyocd (only when the bootloader is misbehaving, or for first
  bootloader install / re-install):
    west flash -d build/macro_keyboard/m3 -r pyocd
    pyocd will flash zmk.hex directly to 0x26000 — bootloader area
    (0x0..0x25FFF, 0xF4000..0x100000) is left untouched.

[BOOTLOADER]
A UF2 bootloader (Adafruit_nRF52_Bootloader, `nice_nano_v2` build — same
SoC family as the MDBT50Q-1MV2 module, no external QSPI) is the default
flashing target. The binary lives at:
  firmware/bootloader/macro_keyboard_bootloader.hex

The build is wired for it by default — see macro_keyboard_defconfig:
  CONFIG_USE_DT_CODE_PARTITION=y       (links app at code_partition = 0x26000)
  CONFIG_BUILD_OUTPUT_UF2=y             (emits zmk.uf2)
  CONFIG_BUILD_OUTPUT_UF2_FAMILY_ID="0xADA52840"   (Adafruit nRF52840 family ID)

DTS partition layout (in macro_keyboard.dts, &flash0):
  0x000000 – 0x025FFF  reserved by bootloader (MBR + bootloader header)
  0x026000 – 0x0EBFFF  code_partition     (~792 KB)
  0x0EC000 – 0x0F3FFF  storage_partition  (32 KB NVS)
  0x0F4000 – 0x0FFFFF  reserved for bootloader code

Bootloader install / re-install (one-time, via SWD/DAPLink):
  pyocd flash --target=nrf52840 \
    /Users/nikolaskelava/Documents/macro_keyboard/firmware/bootloader/macro_keyboard_bootloader.hex
After this, double-tapping reset mounts the USB MSC drive.

If the bootloader has to be removed (full chip erase, no UF2):
  Revert macro_keyboard_defconfig (drop USE_DT_CODE_PARTITION + the two
  BUILD_OUTPUT_UF2 lines), revert the DTS partition layout to start at
  0x000000 with size 0xF0000, pristine-rebuild, and flash via SWD. The
  bootloader is then gone — drag-drop UF2 will not work until reinstalled.

DANGER:
  USE_DT_CODE_PARTITION=y means the firmware's reset vector lives at
  0x26000, NOT 0x0. If the bootloader is ever erased without also reverting
  this flag, the CPU resets into an empty vector table at 0x0 and the
  board appears bricked (SWD still works to recover; either reinstall the
  bootloader or revert the flag and reflash).

[IMPLEMENTATION STRATEGY — milestones]

Milestone 1 — Build reliably.   [DONE]
  Board recognised by Zephyr/ZMK (board.yml, Kconfig.*, DTS, defconfig);
  matrix scans 3x4; profile button wired (input-only at this stage); dev
  + studio variants build pristinely.

Milestone 2 — UI infra + battery sense.   [DONE]
  I2C0 enabled (P0.24 SDA / P0.25 SCL); ssd1306@0x3c node + chosen
  zephyr,display = &oled; CONFIG_ZMK_DISPLAY=y enables LVGL; battery
  voltage-divider on AIN2 with CONFIG_ZMK_BATTERY_REPORTING=y. Visuals
  not yet exercised on hardware.

Milestone 3 — Profiles + profile-button cycle.   [DONE]
  - macro_keyboard.keymap defines 4 layers (media / productivity / browser / bt).
    Each layer's pos-12 binding is `&profile_next`.
  - `&profile_next` is a custom ZMK behavior in module/src/behavior_profile_next.c
    that reads zmk_keymap_highest_layer_active() and advances mod 4 — so
    cycling is consistent regardless of which layer's binding fires.
  - Profile button uses kscan-composite combining the 12-key matrix
    (zmk,kscan-gpio-matrix) and a 1-key direct kscan (zmk,kscan-gpio-direct
    on P1.02) at row_offset=3. Matrix-transform is 4r x 4c with map[12] =
    RC(3,0). The diode-clamped pressed voltage (~0.7V) is marginal for
    nRF52 GPIO interrupts, so CONFIG_ZMK_KSCAN_DIRECT_POLLING=y polls
    instead. Matrix kscan unaffected.
  - Physical layout places the profile button at (300, 0) — above the top-
    right matrix key — so Studio renders it where the user actually presses.
  - `&studio_unlock` retained on layer 0 key 8 (FR §4: at least one layer).
  - macOS-style shortcuts: productivity / browser layers use LG (Cmd), not
    LC (Ctrl). Edit in Studio for non-Mac hosts.
  - Hardware-verified: profile cycling works; per-layer keypresses produce
    correct HID output. Studio's active-layer indicator is currently not
    showing — deferred to M6.

Milestone 4 — Display.   [PARTIALLY DONE — BUILT_IN screen works, CUSTOM hangs]

  Working today (macro_keyboard.conf has CONFIG_ZMK_DISPLAY=y only):
  - ZMK's stock BUILT_IN status screen renders (battery widget + layer-name
    label + output-status icon). Profile cycling updates the layer-name
    text. Board enumerates over USB, pairs over BLE, works in Studio.
  - This satisfies the "battery indicator + active profile" half of the FR
    but NOT "show all 4 profiles with active marked" — the custom screen
    below is what would close that gap.

  Five Kconfig settings are load-bearing (all pinned in board
  Kconfig.defconfig under if LVGL / if ZMK_DISPLAY). Removing ANY of them
  silently hangs the firmware at boot and BLE/USB never enumerate:
    1. LV_COLOR_DEPTH_1 + LV_Z_BITS_PER_PIXEL=1 — match the 1bpp SSD1306.
       Default RGB565/32bpp makes LVGL allocate a framebuffer too large
       for the LV mem pool; you see noise + hang.
    2. LV_Z_VDB_SIZE=64 — Zephyr default of 10% trickles partial flushes
       faster than I2C can serve; display work queue starves.
    3. ZMK_DISPLAY_WORK_QUEUE_DEDICATED — isolates display work from the
       system work queue (where BLE advertising + USB-HID TX run). Without
       this any LVGL stall takes BLE/USB down with it.
    4. LV_USE_THEME_MONO=y — BUILT_IN auto-implies it, CUSTOM does not.
       Without it LVGL has no default font on the display, lv_label_create
       renders with NULL font → hardfault → BLE/USB die too.
    5. LV_FONT_MONTSERRAT_16 + LV_FONT_DEFAULT_MONTSERRAT_16 — same story:
       BUILT_IN sets them inside its `if` block; CUSTOM doesn't.

  Custom status screen — OPEN.
  - module/src/status_screen.c exists, gated on
    CONFIG_ZMK_DISPLAY_STATUS_SCREEN_CUSTOM in module/CMakeLists.txt.
  - Flipping the choice to CUSTOM in macro_keyboard.conf reproduces the
    boot hang even with a MINIMAL screen body (just `lv_obj_create(NULL)`
    + one `lv_label_create` + `lv_label_set_text` + return). Same symptom
    as the original BPP bug: 8-row page noise on the OLED, no USB/BLE.
  - With all 5 settings above in place this should NOT happen — at this
    point the Kconfig effective values match BUILT_IN's exactly. Cause is
    structural and not yet identified. See "[M4 — open question]" below.

Milestone 5 — AS5600 magnetic encoder.   [PENDING]
  - I2C0 @ 0x36, shared with the OLED. No upstream ZMK driver exists.
  - Implement as a Zephyr sensor driver inside module/drivers/sensor/as5600/,
    converting raw angle deltas (0x0E/0x0F registers) into discrete tick
    events and emitting them as SENSOR_CHAN_ROTATION changes that ZMK's
    sensor-binding plumbing can consume.
  - Add per-layer `sensor-bindings = <...>` in macro_keyboard.keymap:
    P1=volume, P2=vertical scroll, P3=horizontal scroll, P4=volume.
  - Verify each profile's encoder action on hardware.

Milestone 6 — Power + ZMK Studio polish.   [PENDING]
  - Verify CONFIG_ZMK_SLEEP behaviour with display + I2C peripherals
    (display blanks; I2C drops to low-power pinctrl on idle).
  - Investigate Studio active-layer indicator not updating
    (Studio sees the 4 layers, but doesn't visibly highlight which is
    active — likely needs ZMK_KEYMAP_LAYER_REORDERING or Studio-side state
    push; check latest ZMK docs).
  - Confirm BLE pairing across multiple hosts; verify divider scaling on
    battery widget against measured pack voltage.

[TOOLCHAIN]
Native Zephyr/ZMK install on M1 Mac (followed the ZMK "Getting Started" native setup guide).
Toolchain at: /firmware/zmk_toolchain/{app,modules,zephyr}
Always: source /firmware/zmk_toolchain/.venv/bin/activate before building.

[BOARD FILES — current state]
Vendor/name: nikolas / macro_keyboard  (HWMv2, SoC nrf52840)
Files under zmk-config/config/boards/arm/macro_keyboard/:
  board.yml, board.cmake, pre_dt_board.cmake,
  Kconfig.macro_keyboard, Kconfig.defconfig,
  macro_keyboard.yaml, macro_keyboard_defconfig,
  macro_keyboard.dts
Keymap + config (user-side, picked up via ZMK_CONFIG):
  zmk-config/config/macro_keyboard.keymap  (4 layers, profile button on pos 12)
  zmk-config/config/macro_keyboard.conf
DTS wiring (matches [PINMAP]):
  kscan_matrix    : zmk,kscan-gpio-matrix, col2row, rows P0.20/21/19, cols P0.16/14/15/13
  kscan_direct    : zmk,kscan-gpio-direct, P1.02 (profile button)
  kscan_composite : combines both into a single 4r x 4c kscan;
                    zmk,kscan = &kscan_composite (chosen)
  matrix xform    : matrix_transform0 (4r x 4c, sparse — 13 entries)
  phys layout     : physical_layout0 (13 keys, profile btn at (300, 0))
  i2c0            : P0.24 SDA / P0.25 SCL, fast mode; oled@0x3c node;
                    BUILT_IN status screen rendering as of M4 partial-done
  adc             : enabled; vbatt on AIN2 (P0.04) via zmk,battery-voltage-divider
                    (divider values are placeholders — verify in M6)

[CURRENT FLASH PARTITION LAYOUT — UF2 bootloader installed]
  0x000000 – 0x025FFF  reserved by bootloader (MBR + bootloader header)
  0x026000 – 0x0EBFFF  code_partition     (~792 KB ZMK app — links here)
  0x0EC000 – 0x0F3FFF  storage_partition  (32 KB NVS)
  0x0F4000 – 0x0FFFFF  reserved for bootloader code
See [BOOTLOADER] above for the install procedure and the (rare) revert
path back to direct-SWD.

[M4 — open question: CUSTOM status screen hangs the firmware]

Symptom on flip to CONFIG_ZMK_DISPLAY_STATUS_SCREEN_CUSTOM=y:
  - OLED shows 8-row-page random noise (SSD1306 GDDRAM contents on
    power-up, never overwritten by LVGL).
  - Board does NOT enumerate over USB, does NOT advertise over BLE.
  - Mac does not heat / no USB-retry storm — looks like a clean early hang.
  - Reproduces with a MINIMAL custom screen body:
      lv_obj_t *s = lv_obj_create(NULL);
      lv_obj_t *l = lv_label_create(s);
      lv_label_set_text(l, "test");
      return s;
  - Reverting to BUILT_IN (current state) makes it all work.

What's been ruled out:
  - All 5 Kconfig settings listed in M4 are confirmed present in the
    failing build's .config (and matching the working BUILT_IN .config).
    LV_USE_THEME_MONO=y, LV_FONT_DEFAULT_MONTSERRAT_16=y, both fonts
    linked in the elf, lv_theme_mono_init linked.
  - Verified our zmk_display_status_screen symbol is non-weak in T section
    of the elf (overrides main.c's weak default).
  - Dedicated work queue is in effect (ZMK_DISPLAY_WORK_QUEUE_DEDICATED=y).
    With BUILT_IN the dedicated queue protects BLE/USB even if display
    misbehaves — yet with CUSTOM the kernel goes down too, which strongly
    suggests a CPU fault (hardfault) in the display thread, not a stall.

What hasn't been tried yet:
  - Confirm via objdump / disassembly that initialize_theme()'s call to
    lv_theme_mono_init actually runs (could be inlined or short-circuited).
  - Wire a UART console to the board for boot-time logs / fault traces
    (currently there is none — that's the biggest debug-pain blocker).
  - Compare actual .config diff between BUILT_IN and CUSTOM builds line by
    line to spot any other implicit selects we still missed (the previous
    diffs only caught LV_USE_THEME_MONO and the fonts).
  - Try ZMK_DISPLAY_WORK_QUEUE_SYSTEM (the system queue) with CUSTOM —
    if it fails the same way, it's not a queue/scheduler interaction.
  - Look at `lv_obj_create(NULL)` behaviour in LVGL v9 — possibly the
    correct way to make a custom screen has changed and BUILT_IN
    happens to do it differently (it uses the same pattern, so this is
    unlikely, but worth confirming with a sample).

Fallback the user has today: BUILT_IN screen. Battery + current layer
name + output indicator render correctly; the only spec gap is "show
all 4 profiles with active marked".

To resume work:
  - Switch macro_keyboard.conf back to:
      CONFIG_ZMK_DISPLAY_STATUS_SCREEN_CUSTOM=y
      CONFIG_ZMK_WIDGET_BATTERY_STATUS=y
    (the rest of the LVGL-pinned settings are in board Kconfig.defconfig
    and stay applied for both screen choices).
  - module/src/status_screen.c is currently in its MINIMAL diagnostic form
    — just `lv_obj_create + lv_label_create + set_text + align + return`.
    Use that as the starting point for further bisection.
