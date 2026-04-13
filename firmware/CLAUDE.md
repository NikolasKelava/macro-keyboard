# MacroKeyboard_Zmk — CLAUDE.md (macro_keyboard)
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
Profile button: PROFILE=P1.02
Battery ADC:    VD_ADC=P0.04
(AS5600 OUT pin exists on PCB: ENC_OUT=P0.29; use only if needed in future revisions)

[REPO LAYOUT]
Workspace:
  /firmware/zmk_toolchain/{app,modules,zephyr}
User config:
  /firmware/zmk-config/config/
Board files:
  /firmware/zmk-config/config/boards/arm/macro_keyboard/

[LOCAL BUILDS — ALWAYS]
All build commands must be run from:
  /firmware/zmk_toolchain/app
Use a distinct build directory per build (default rule):
  build/<board>/<variant>/
Examples:
  build/macro_keyboard/dev/
  build/macro_keyboard/studio/
  build/macro_keyboard/test_matrix/

Base build (pristine):
  west build -p -d build/macro_keyboard/dev -b macro_keyboard -- \
    -DZMK_CONFIG="/Users/nikolaskelava/Documents/macro_keyboard/firmware/zmk-config/config"

Incremental rebuild:
  west build -d build/macro_keyboard/dev

ZMK Studio build (enable Studio per latest docs; include snippet if required by docs):
  west build -p -d build/macro_keyboard/studio -b macro_keyboard -S studio-rpc-usb-uart -- \
    -DZMK_CONFIG="/Users/nikolaskelava/Documents/macro_keyboard/firmware/zmk-config/config" \
    -DCONFIG_ZMK_STUDIO=y

[IMPLEMENTATION STRATEGY]
Milestone 1 (make it build reliably):  [DONE — dev + studio builds clean]
  - Board recognized by Zephyr/ZMK (board.yml, Kconfig.*, DTS, defconfig)
  - Matrix scanning works and keymap matches 3x4
  - Profile button wired as input (even if logic is initially stubbed)
Milestone 2 (UI + sensors):
  - I2C enabled in DTS; OLED node added (can be disabled if driver/bindings block the build)
  - Battery ADC channel available
Milestone 3 (behavior):
  - Profiles drive layer/behavior selection
  - Encoder/AS5600 integrated as supported by available ZMK/Zephyr drivers or via a custom module if needed
  - Display reflects battery + active profile

[TOOLCHAIN]
Native Zephyr/ZMK install on M1 Mac (followed the ZMK "Getting Started" native setup guide).
Toolchain at: /firmware/zmk_toolchain/{app,modules,zephyr}

[MILESTONE 1 — CURRENT STATE]
Board vendor/name: nikolas / macro_keyboard  (HWMv2, SoC nrf52840)
Files under zmk-config/config/boards/arm/macro_keyboard/:
  board.yml, board.cmake, pre_dt_board.cmake,
  Kconfig.macro_keyboard, Kconfig.defconfig,
  macro_keyboard.yaml, macro_keyboard_defconfig,
  macro_keyboard.dts
Keymap + config (user-side, picked up via ZMK_CONFIG):
  zmk-config/config/macro_keyboard.keymap  (single 3x4 default_layer, &studio_unlock on key 8)
  zmk-config/config/macro_keyboard.conf    (CONFIG_ZMK_BLE/USB/STUDIO/SLEEP)
DTS wiring (matches [PINMAP]):
  kscan0      : zmk,kscan-gpio-matrix, col2row, rows P0.20/21/19, cols P0.16/14/15/13
  matrix xform: matrix_transform0 (3r x 4c, row-major)
  phys layout : physical_layout0 (12 keys, Studio-ready) — chosen zmk,physical-layout
  i2c0        : P0.24 SDA / P0.25 SCL, fast mode; oled@0x3c node present but status="disabled"
  adc         : enabled; vbatt on AIN2 (P0.04) via zmk,battery-voltage-divider (divider values are placeholders)
  profile_btn : gpio-keys on P1.02, active-low + pull-up (input only, no behavior bound yet)
Flash partition layout (direct SWD, no bootloader):
  code_partition    @ 0x000000, size 0xF0000  (code links from 0x0)
  storage_partition @ 0x0F0000, size 0x10000  (NVS settings)
  WARNING: Do NOT add CONFIG_USE_DT_CODE_PARTITION=y — that links from 0x26000
  (UF2-bootloader offset) and the board will hang on reset with no valid vector table.

[FLASHING — pyocd / DAPLink]
Runner is configured in board.cmake (pyocd, --target=nrf52840).
From /firmware/zmk_toolchain/app, after a successful build:
  west flash -d build/macro_keyboard/studio -r pyocd
Artifact for manual flashing: build/macro_keyboard/studio/zephyr/zmk.hex