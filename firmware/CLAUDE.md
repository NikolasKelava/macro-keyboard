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
  - drivers/sensor/as5600/...     : AS5600 encoder driver (M5)
  - src/encoder_mode.c            : per-profile encoder-mode state (M5)
  - src/behavior_encoder_dispatch.c   : &enc_dispatch sensor behavior (M5)
  - src/behavior_encoder_mode_next.c  : &enc_mode_next key behavior (M5)
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

Milestone 4 — Display.   [DONE]

  macro_keyboard.conf selects CUSTOM:
    CONFIG_ZMK_DISPLAY=y
    CONFIG_ZMK_DISPLAY_STATUS_SCREEN_CUSTOM=y
    CONFIG_ZMK_WIDGET_BATTERY_STATUS=y   (kept to pull LV_USE_LABEL in)
  module/src/status_screen.c is the screen body, gated on
  CONFIG_ZMK_DISPLAY_STATUS_SCREEN_CUSTOM in module/CMakeLists.txt.

  Layout (logical 64w x 128h portrait — the panel is physically 128x64
  landscape and we rotate via a custom flush callback, see "Rotation"):
    - battery row, centred horizontally at the top of the rotated canvas:
      a custom battery icon ("[==] NN%"). Drawn crisp for the 1bpp panel —
      LVGL renders the 4bpp anti-aliased Montserrat font (and the
      FontAwesome LV_SYMBOL battery/charge glyphs baked into it) by
      thresholding luminance at 127 onto I1, which makes them bulky/fuzzy
      with stray edge pixels. So the row uses NO fonts/symbols for the
      icon: the percentage is lv_font_unscii_8 (true 1bpp, 8 px, no AA)
      and the battery is full-opacity axis-aligned rectangles (body
      outline + nub + level-proportional fill). While charging the interior
      is filled solid and a 7x9 lightning bolt (hand-drawn 1bpp lv_canvas,
      OFF/dark pixels on the lit fill, spanning the full interior height)
      is overlaid as a cut-out.
      battery_update re-centres the whole row each update. NOT a polarity
      bug — polarity is correct (else the whole screen would invert).
      Requires LV_FONT_UNSCII_8=y and LV_USE_CANVAS=y (pinned in board
      Kconfig.defconfig). Subscribes to zmk_battery_state_changed +
      zmk_usb_conn_state_changed for live cable/SoC updates.
    - centred column below: four 52x22 cells (radius 8) stacked vertically
      (6 px gap, first cell top at y=16, last cell bottom at y=122). The
      6 px left/right margin and 6 px bottom margin to the screen edge are
      equal by design. Each cell is an lv_obj container; default style is a
      rounded outline with transparent fill, active style (LV_STATE_CHECKED)
      is a solid fill with an inverted text_color the child digit label
      inherits. The digit uses lv_font_unscii_16 (1bpp) so the active
      (inverted) digit's stems stay pixel-identical to the inactive ones —
      a 4bpp font thresholds asymmetrically on inversion and the selected
      digit renders ~1 px thinner. profile_update toggles LV_STATE_CHECKED.
      Cells need to be containers (not bare labels with bg styles) because
      in LVGL v9 lv_label doesn't draw its own background reliably.
    - "active profile" is derived from zmk_keymap_highest_layer_active(),
      same source the profile_next behavior advances. Layers 0..3 map 1:1
      to profiles 1..4; indices outside that range fall back to slot 0.

  Both widgets use ZMK_DISPLAY_WIDGET_LISTENER (see app/include/zmk/display.h),
  which marshals state updates from the system work queue onto the dedicated
  display work queue under a mutex — same pattern used by ZMK's stock
  battery_status / layer_status widgets.

  Rotation. LVGL v9's lv_display_set_rotation() only updates the logical
  resolution; Zephyr's lvgl_flush_cb_mono writes LVGL's logical coords
  straight to the panel and does not transform pixels, so on this stack
  the call alone is a no-op for rendering. status_screen.c installs its
  own rotated_flush_cb via lv_display_set_flush_cb after rotation is set:
  the callback walks the LVGL 1bpp pixel buffer bit by bit, rotates
  270° CW into a static 1100-byte scratch buffer, recomputes the area
  from logical (64x128) to physical (128x64) coordinates, then delegates
  to lvgl_flush_cb_mono (declared extern from
  zephyr/modules/lvgl/lvgl_display_mono.c) which handles the SSD1306
  page-major packing. Direction (270° vs 90°) was picked by hardware
  test; the device is mounted with the panel's physical left edge at the
  user's top. The default lvgl_rounder_cb_mono is reused unchanged
  because it aligns both x and y to 8 (the SSD1306 is MONO_VTILED), and
  both axes stay 8-aligned through our rotation.

  Polarity. The DTS has no `inversion-on`, so the SSD1306 driver reports
  PIXEL_FORMAT_MONO01 — in that format `lv_color_white()` maps to an OFF
  pixel and `lv_color_black()` to a LIT pixel. theme_mono is calibrated
  for this convention, so the screen leaves bg/text to the theme and
  only swaps white<->black inside the cell styles where we want the
  inverse (lit cell outlines/fill, OFF digit when active).

  Six Kconfig settings are load-bearing (all pinned in board
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
    6. LV_Z_MEM_POOL_SIZE=12288 — ZMK's app/src/display/Kconfig sets this
       default to 4096 only when STATUS_SCREEN_BUILT_IN is selected. With
       CUSTOM it falls back to Zephyr's 2048-byte default — not enough for
       even theme_mono + screen + one label, so lv_obj_create silently
       returns NULL and the next deref hardfaults, taking BLE/USB down.
       8192 covered the original M4 layout; the crisp battery-icon rework
       added the body/nub/fill objects + the charge-bolt canvas, so we
       pin 12288 in board Kconfig.defconfig with measured headroom.
       (~4 KB extra RAM, well within budget.)

Milestone 5 — AS5600 magnetic encoder.   [DONE]

  Driver (module/drivers/sensor/as5600/). Custom DT compatible
  `nikolas,as5600` — NOT Zephyr's built-in `ams,as5600`, which would
  double-instantiate (Zephyr ships a driver for that compatible). The
  AS5600 has no IRQ wired to the MCU (its OUT pad P0.29 is unused), so the
  driver POLLS: a dedicated thread (kept off the system work queue so a
  blocked I2C read on the OLED-shared bus can't stall BLE/USB) reads RAW
  ANGLE (0x0E/0x0F) every poll-period-ms (5), and synthesises the
  SENSOR_TRIG_DATA_READY trigger ZMK's keymap-sensors subsystem requires.
  It reports rotation **delta in degrees** on SENSOR_CHAN_ROTATION (val1
  whole, val2 micro), only once ≥1° has accumulated (carrying the
  remainder) — a report with val1==0 hits a legacy "val2 is a raw tick
  count" path in ZMK's sensor-rotate behaviour.
  Two signal-conditioning stages (both Kconfig, in the driver's Kconfig):
    - AS5600_SMOOTHING_SHIFT (1): light wrap-aware EMA low-pass on the angle.
    - AS5600_DEADBAND_COUNTS (4): ignore motion within N counts of a
      reference angle; advance the reference only on real movement.
  The deadband is LOAD-BEARING, not cosmetic: a still magnet jitters ±1-2
  LSB on RAW angle; without the deadband that random-walks past the 1°
  report threshold ~1×/s, raising spurious sensor events. ZMK's activity
  monitor (app/src/activity.c) subscribes to zmk_sensor_event, so those
  events reset the idle timer — the board never idles (display never
  blanks, never deep-sleeps) AND the constant sensor→behaviour→HID churn
  eventually hung the board over USB. Adding the deadband fixed both.
  Also load-bearing: AS5600_STARTUP_DELAY_MS (1000) — the thread waits
  before its first I2C read so the OLED/BLE/USB finish init first (touching
  the shared bus during OLED init was an intermittent boot hang); and
  AS5600_THREAD_STACK_SIZE (2048) — the thread is the context in which the
  whole sensor→behaviour→HID chain runs synchronously.

  Per-profile mode (custom firmware — required because ZMK STUDIO CANNOT
  EDIT ENCODER/SENSOR BINDINGS in this ZMK version: its keymap proto is
  key-position only, no sensor messages). encoder_mode.c holds one mode per
  profile (= per keymap layer) in RAM: volume / vertical-scroll /
  horizontal-scroll, defaults {vol, vscroll, hscroll, vol}. `&enc_dispatch`
  is the sensor-binding on every layer; it reads the active profile's mode
  at each tick and emits the action. `&enc_mode_next` (a key, pos 11 on
  every layer) cycles the current profile's mode. (Persisting the mode
  across reboot is deferred — see M6.)

  Per-mode resolution. `&enc_dispatch` does NOT use the keymap-sensors
  node's triggers-per-rotation (that prop is omitted). It accumulates raw
  micro-degrees and quantises with the CURRENT mode's own value from its DT
  node: `volume-triggers-per-rotation` (14 → coarse, ~25.7°/step) and
  `scroll-triggers-per-rotation` (120 → fine, 3°/tick). The sub-tick
  remainder is carried so mode changes mid-turn are seamless. Practical
  scroll ceiling ≈ 360 (the driver's ~1° report granularity).

  Scroll path. Does NOT use `&msc` (zmk,behavior-input-two-axis) — that's a
  velocity×time model for HELD mouse keys and emits ≈0 per brief tick.
  Instead it reports discrete wheel events straight to the &msc input
  device: input_report_rel(DEVICE_DT_GET(DT_NODELABEL(msc)),
  INPUT_REL_WHEEL/HWHEEL, ±ticks, true, K_NO_WAIT) — one report per sensor
  event (a per-tick loop flooded USB HID TX and hung the board). Needs
  CONFIG_ZMK_POINTING=y + CONFIG_ZMK_POINTING_SMOOTH_SCROLLING=y (high-res,
  via HID Resolution Multiplier). The firmware has NO scroll acceleration;
  the "fast = sudden" ramp was macOS-side and the user disabled it with
  LinearMouse.

  Hardware-verified: volume (14/rev) + V/H scroll (120/rev) per profile,
  mode key cycles per-profile mode, display blanks at rest, no hangs on USB
  or BLE.

Milestone 6 — Power + ZMK Studio polish.   [PENDING]
  - Idle/sleep now WORKS (display blanks at rest, deep-sleep reachable) —
    the AS5600 deadband was the missing piece (see M5). Remaining power
    item: the encoder poll thread wakes every 5 ms forever, which prevents
    the SoC's automatic low-power idle between polls (battery drain). For
    M6, consider gating/slowing the poll on ZMK's activity state.
  - Deferred M5 follow-ups: (a) on-screen OLED encoder-mode indicator —
    needs the M4 status-screen layout reworked; (b) persist the per-profile
    encoder mode across reboot (currently RAM-only in encoder_mode.c).
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
  i2c0            : P0.24 SDA / P0.25 SCL, fast mode; oled@0x3c (CUSTOM
                    status screen, M4) + as5600@0x36 (nikolas,as5600, M5)
  sensors         : zmk,keymap-sensors node -> &as5600 (M5; resolution is
                    per-mode on &enc_dispatch, not on this node)
  adc             : enabled; vbatt on AIN2 (P0.04) via zmk,battery-voltage-divider
                    (divider values are placeholders — verify in M6)

[CURRENT FLASH PARTITION LAYOUT — UF2 bootloader installed]
  0x000000 – 0x025FFF  reserved by bootloader (MBR + bootloader header)
  0x026000 – 0x0EBFFF  code_partition     (~792 KB ZMK app — links here)
  0x0EC000 – 0x0F3FFF  storage_partition  (32 KB NVS)
  0x0F4000 – 0x0FFFFF  reserved for bootloader code
See [BOOTLOADER] above for the install procedure and the (rare) revert
path back to direct-SWD.

[M4 — resolved: CUSTOM hang root cause was LV_Z_MEM_POOL_SIZE]

The "all 5 Kconfig settings match BUILT_IN exactly and CUSTOM still hangs"
puzzle was solved by diffing a pristine CUSTOM .config against the
working BUILT_IN .config. The only meaningful difference (beyond the
screen choice and the widgets we don't enable) was:
  CONFIG_LV_Z_MEM_POOL_SIZE: 4096 (BUILT_IN) vs 2048 (CUSTOM)
ZMK's app/src/display/Kconfig has `default 4096 if STATUS_SCREEN_BUILT_IN`
on that symbol — so flipping to CUSTOM silently drops the LVGL allocator
pool to Zephyr's 2048-byte default. theme_mono + screen + label can't
all fit, lv_mem_alloc returns NULL, and the next deref hardfaults the
display thread — which the kernel can't recover from since several
init-time submitted works share the same crashed context, explaining
why BLE/USB never enumerate either.

Fix: pin `config LV_Z_MEM_POOL_SIZE default 4096` in board
Kconfig.defconfig under `if LVGL` so it applies regardless of screen
choice. After the fix the pristine CUSTOM .config differs from BUILT_IN
only in the intended places (screen choice + widget enables).

Hardware verification still pending — the dev + studio CUSTOM UF2s are
built but not yet flashed. UART console + objdump steps from the
earlier "haven't tried yet" list are no longer necessary unless flashing
reveals a separate issue.
