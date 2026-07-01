# MacroKeyboard — Implementation Plan & Milestone History

> This file is the historical record of how `macro_keyboard` was built: the
> **initial functional requirements** the project started from, and the
> **milestone-by-milestone implementation strategy** that realised them.
>
> For the **current** device description, functional requirements, pinmap,
> build/flash procedure, and the distilled load-bearing constraints, see
> [CLAUDE.md](./CLAUDE.md). Where the initial requirements below differ from
> the current behaviour (e.g. a fixed 4 profiles became a flexible 1–7), the
> current behaviour in CLAUDE.md wins — this file preserves the baseline the
> plan was designed against.

## Initial Functional Requirements — Behavioral Overview

*(The original requirements at project start. Retained as the baseline; some
have since evolved — see CLAUDE.md for current behaviour.)*

1. **Connectivity**
   - Acts as a BLE HID keyboard/media device; should work OS-agnostically (Windows/macOS/Linux/iOS/Android) via standard HID.
2. **Profiles (4 total)**
   - A single hardware button cycles active profile 1→2→3→4→1.
   - Each profile defines:
     - Key macros (media keys, shortcuts, app-launch sequences where supported by host OS)
     - Encoder behavior (volume or scroll; direction/axis can differ per profile)
3. **Display**
   - Always shows:
     - Battery indicator (percentage/level derived from ADC measurement)
     - Profile list 1–4, with active profile visually marked
   - Display updates on profile change and periodically for battery status.
4. **Macros / Keymap editing**
   - All macros and key bindings must be editable via ZMK Studio.
   - Ensure the configuration includes whatever is required by the current ZMK Studio workflow (per latest ZMK docs).
5. **Power**
   - Battery ADC reading is exposed to the firmware (and optionally to UI).
   - Aim for low-power behavior consistent with ZMK defaults (sleep/idle as supported).

## Implementation Strategy — Milestones

### Milestone 1 — Build reliably.   [DONE]

Board recognised by Zephyr/ZMK (board.yml, Kconfig.*, DTS, defconfig); matrix scans 3x4; profile button wired (input-only at this stage); dev + studio variants build pristinely.

### Milestone 2 — UI infra + battery sense.   [DONE]

I2C0 enabled (P0.24 SDA / P0.25 SCL); ssd1306@0x3c node + chosen `zephyr,display = &oled`; `CONFIG_ZMK_DISPLAY=y` enables LVGL; battery voltage-divider on AIN2 with `CONFIG_ZMK_BATTERY_REPORTING=y`. Visuals not yet exercised on hardware.

### Milestone 3 — Profiles + profile-button cycle.   [DONE]

- `macro_keyboard.keymap` defines 4 layers (media / productivity / browser / bt). Each layer's pos-12 binding is `&profile_next`.
- `&profile_next` is a custom ZMK behavior in `module/src/behavior_profile_next.c` that reads `zmk_keymap_highest_layer_active()` and advances mod 4 — so cycling is consistent regardless of which layer's binding fires.
- Profile button uses kscan-composite combining the 12-key matrix (`zmk,kscan-gpio-matrix`) and a 1-key direct kscan (`zmk,kscan-gpio-direct` on P1.02) at row_offset=3. Matrix-transform is 4r x 4c with map[12] = RC(3,0). The diode-clamped pressed voltage (~0.7V) is marginal for nRF52 GPIO interrupts, so `CONFIG_ZMK_KSCAN_DIRECT_POLLING=y` polls instead. Matrix kscan unaffected.
- Physical layout places the profile button at (300, 0) — above the top-right matrix key — so Studio renders it where the user actually presses.
- `&studio_unlock` retained on layer 0 key 8 (FR §4: at least one layer).
- macOS-style shortcuts: productivity / browser layers use LG (Cmd), not LC (Ctrl). Edit in Studio for non-Mac hosts.
- Hardware-verified: profile cycling works; per-layer keypresses produce correct HID output. Studio's active-layer indicator is currently not showing — deferred to M6.

### Milestone 4 — Display.   [DONE]

`macro_keyboard.conf` selects CUSTOM:

```
CONFIG_ZMK_DISPLAY=y
CONFIG_ZMK_DISPLAY_STATUS_SCREEN_CUSTOM=y
CONFIG_ZMK_WIDGET_BATTERY_STATUS=y   (kept to pull LV_USE_LABEL in)
```

`module/src/status_screen.c` is the screen body, gated on `CONFIG_ZMK_DISPLAY_STATUS_SCREEN_CUSTOM` in `module/CMakeLists.txt`.

**Layout** (logical 64w x 128h portrait — the panel is physically 128x64 landscape and we rotate via a custom flush callback, see "Rotation"):

- battery row, centred horizontally at the top of the rotated canvas: a custom battery icon ("[==] NN%"). Drawn crisp for the 1bpp panel — LVGL renders the 4bpp anti-aliased Montserrat font (and the FontAwesome LV_SYMBOL battery/charge glyphs baked into it) by thresholding luminance at 127 onto I1, which makes them bulky/fuzzy with stray edge pixels. So the row uses NO fonts/symbols for the icon: the percentage is lv_font_unscii_8 (true 1bpp, 8 px, no AA) and the battery is full-opacity axis-aligned rectangles (body outline + nub + level-proportional fill). While charging the interior is filled solid and a 7x9 lightning bolt (hand-drawn 1bpp lv_canvas, OFF/dark pixels on the lit fill, spanning the full interior height) is overlaid as a cut-out. battery_update re-centres the whole row each update. NOT a polarity bug — polarity is correct (else the whole screen would invert). Requires `LV_FONT_UNSCII_8=y` and `LV_USE_CANVAS=y` (pinned in board Kconfig.defconfig). Subscribes to zmk_battery_state_changed + zmk_usb_conn_state_changed for live cable/SoC updates.
- centred column below: four 52x22 cells (radius 8) stacked vertically (6 px gap, first cell top at y=16, last cell bottom at y=122). The 6 px left/right margin and 6 px bottom margin to the screen edge are equal by design. Each cell is an lv_obj container; default style is a rounded outline with transparent fill, active style (LV_STATE_CHECKED) is a solid fill with an inverted text_color the child digit label inherits. The digit uses lv_font_unscii_16 (1bpp) so the active (inverted) digit's stems stay pixel-identical to the inactive ones — a 4bpp font thresholds asymmetrically on inversion and the selected digit renders ~1 px thinner. profile_update toggles LV_STATE_CHECKED. Cells need to be containers (not bare labels with bg styles) because in LVGL v9 lv_label doesn't draw its own background reliably.
- "active profile" is derived from `zmk_keymap_highest_layer_active()`, same source the profile_next behavior advances. Layers 0..3 map 1:1 to profiles 1..4; indices outside that range fall back to slot 0.

Both widgets use ZMK_DISPLAY_WIDGET_LISTENER (see `app/include/zmk/display.h`), which marshals state updates from the system work queue onto the dedicated display work queue under a mutex — same pattern used by ZMK's stock battery_status / layer_status widgets.

**Rotation.** LVGL v9's `lv_display_set_rotation()` only updates the logical resolution; Zephyr's `lvgl_flush_cb_mono` writes LVGL's logical coords straight to the panel and does not transform pixels, so on this stack the call alone is a no-op for rendering. `status_screen.c` installs its own rotated_flush_cb via `lv_display_set_flush_cb` after rotation is set: the callback walks the LVGL 1bpp pixel buffer bit by bit, rotates 270° CW into a static 1100-byte scratch buffer, recomputes the area from logical (64x128) to physical (128x64) coordinates, then delegates to `lvgl_flush_cb_mono` (declared extern from `zephyr/modules/lvgl/lvgl_display_mono.c`) which handles the SSD1306 page-major packing. Direction (270° vs 90°) was picked by hardware test; the device is mounted with the panel's physical left edge at the user's top. The default `lvgl_rounder_cb_mono` is reused unchanged because it aligns both x and y to 8 (the SSD1306 is MONO_VTILED), and both axes stay 8-aligned through our rotation.

**Polarity.** The DTS has no `inversion-on`, so the SSD1306 driver reports PIXEL_FORMAT_MONO01 — in that format `lv_color_white()` maps to an OFF pixel and `lv_color_black()` to a LIT pixel. theme_mono is calibrated for this convention, so the screen leaves bg/text to the theme and only swaps white<->black inside the cell styles where we want the inverse (lit cell outlines/fill, OFF digit when active).

**Six Kconfig settings are load-bearing** (all pinned in board Kconfig.defconfig under `if LVGL` / `if ZMK_DISPLAY`). Removing ANY of them silently hangs the firmware at boot and BLE/USB never enumerate:

1. `LV_COLOR_DEPTH_1` + `LV_Z_BITS_PER_PIXEL=1` — match the 1bpp SSD1306. Default RGB565/32bpp makes LVGL allocate a framebuffer too large for the LV mem pool; you see noise + hang.
2. `LV_Z_VDB_SIZE=64` — Zephyr default of 10% trickles partial flushes faster than I2C can serve; display work queue starves.
3. `ZMK_DISPLAY_WORK_QUEUE_DEDICATED` — isolates display work from the system work queue (where BLE advertising + USB-HID TX run). Without this any LVGL stall takes BLE/USB down with it.
4. `LV_USE_THEME_MONO=y` — BUILT_IN auto-implies it, CUSTOM does not. Without it LVGL has no default font on the display, lv_label_create renders with NULL font → hardfault → BLE/USB die too.
5. `LV_FONT_MONTSERRAT_16` + `LV_FONT_DEFAULT_MONTSERRAT_16` — same story: BUILT_IN sets them inside its `if` block; CUSTOM doesn't.
6. `LV_Z_MEM_POOL_SIZE=12288` — ZMK's `app/src/display/Kconfig` sets this default to 4096 only when STATUS_SCREEN_BUILT_IN is selected. With CUSTOM it falls back to Zephyr's 2048-byte default — not enough for even theme_mono + screen + one label, so lv_obj_create silently returns NULL and the next deref hardfaults, taking BLE/USB down. 8192 covered the original M4 layout; the crisp battery-icon rework added the body/nub/fill objects + the charge-bolt canvas, so we pin 12288 in board Kconfig.defconfig with measured headroom. (~4 KB extra RAM, well within budget.)

#### M4 — Resolved: CUSTOM Hang Root Cause Was LV_Z_MEM_POOL_SIZE

The "all 5 Kconfig settings match BUILT_IN exactly and CUSTOM still hangs" puzzle was solved by diffing a pristine CUSTOM `.config` against the working BUILT_IN `.config`. The only meaningful difference (beyond the screen choice and the widgets we don't enable) was:

```
CONFIG_LV_Z_MEM_POOL_SIZE: 4096 (BUILT_IN) vs 2048 (CUSTOM)
```

ZMK's `app/src/display/Kconfig` has `default 4096 if STATUS_SCREEN_BUILT_IN` on that symbol — so flipping to CUSTOM silently drops the LVGL allocator pool to Zephyr's 2048-byte default. theme_mono + screen + label can't all fit, lv_mem_alloc returns NULL, and the next deref hardfaults the display thread — which the kernel can't recover from since several init-time submitted works share the same crashed context, explaining why BLE/USB never enumerate either.

Fix: pin `config LV_Z_MEM_POOL_SIZE default 4096` in board Kconfig.defconfig under `if LVGL` so it applies regardless of screen choice. After the fix the pristine CUSTOM `.config` differs from BUILT_IN only in the intended places (screen choice + widget enables). (Later bumped to 12288 for the battery-icon rework — see load-bearing setting #6 above.)

### Milestone 5 — AS5600 magnetic encoder.   [DONE]

**Driver** (`module/drivers/sensor/as5600/`). Custom DT compatible `nikolas,as5600` — NOT Zephyr's built-in `ams,as5600`, which would double-instantiate (Zephyr ships a driver for that compatible). The AS5600 has no IRQ wired to the MCU (its OUT pad P0.29 is unused), so the driver POLLS: a dedicated thread (kept off the system work queue so a blocked I2C read on the OLED-shared bus can't stall BLE/USB) reads RAW ANGLE (0x0E/0x0F) every poll-period-ms (5), and synthesises the SENSOR_TRIG_DATA_READY trigger ZMK's keymap-sensors subsystem requires. It reports rotation **delta in degrees** on SENSOR_CHAN_ROTATION (val1 whole, val2 micro), only once ≥1° has accumulated (carrying the remainder) — a report with val1==0 hits a legacy "val2 is a raw tick count" path in ZMK's sensor-rotate behaviour.

Two signal-conditioning stages (both Kconfig, in the driver's Kconfig):

- `AS5600_SMOOTHING_SHIFT` (1): light wrap-aware EMA low-pass on the angle.
- `AS5600_DEADBAND_COUNTS` (4): ignore motion within N counts of a reference angle; advance the reference only on real movement.

The deadband is LOAD-BEARING, not cosmetic: a still magnet jitters ±1-2 LSB on RAW angle; without the deadband that random-walks past the 1° report threshold ~1×/s, raising spurious sensor events. ZMK's activity monitor (`app/src/activity.c`) subscribes to zmk_sensor_event, so those events reset the idle timer — the board never idles (display never blanks, never deep-sleeps) AND the constant sensor→behaviour→HID churn eventually hung the board over USB. Adding the deadband fixed both.

Also load-bearing: `AS5600_STARTUP_DELAY_MS` (1000) — the thread waits before its first I2C read so the OLED/BLE/USB finish init first (touching the shared bus during OLED init was an intermittent boot hang); and `AS5600_THREAD_STACK_SIZE` (2048) — the thread is the context in which the whole sensor→behaviour→HID chain runs synchronously.

**Per-profile mode** (custom firmware — ZMK STUDIO CANNOT EDIT ENCODER/SENSOR BINDINGS in this ZMK version: keymap proto is key-position only). `encoder_mode.c` holds one mode per profile (= per layer) in RAM: volume / vertical-scroll / horizontal-scroll / TABS. Defaults {vol, vscroll, tabs, vol} (browser profile defaults to tabs). `&enc_dispatch` is the sensor-binding on every layer; it reads the active profile's mode each tick and acts. `&enc_mode_next` (key, pos 11) cycles the mode. (Persisting across reboot deferred — M6.)

**Actions & resolution** (all DT props on `&enc_dispatch`; it quantises accumulated micro-degrees per-mode, carrying the sub-tick remainder):

- volume → `&kp C_VOL_UP/DN`, coarse (volume-triggers-per-rotation 14).
- tabs → `&kp LC(TAB)/LC(LS(TAB))` (next/prev browser tab; Ctrl+Tab works cross-browser on macOS), coarse (tab-triggers-per-rotation 12).
- V/H scroll → wheel reports to the `&msc` input device, finer (scroll-triggers-per-rotation 120). Needs `CONFIG_ZMK_POINTING=y`. NOT the `&msc` *behavior* (a velocity×time model for held keys, ≈0 per tick) — uses input_report_rel on the `&msc` input device.
- CW = volume-up / scroll-up / scroll-right / next-tab.

**Scroll feel is HOST-OWNED, not firmware** (hard-won; don't re-litigate). macOS:

- IGNORES the high-res Resolution Multiplier for this device (SMOOTH_SCROLLING on vs off builds felt identical);
- VELOCITY-CAPS the report RATE — more reports/s does NOT scroll faster (verified with 10× reports, and USB vs BLE: all identical);
- honours per-report MAGNITUDE, but native macOS adds its own accel "jump" and LinearMouse normalises magnitude to a fixed distance.

So firmware emits a CLEAN LINEAR line-delta stream (d_scroll = ticks × scroll-units, scroll-units=1, NO firmware acceleration) and a host smooth-scroll app owns smoothing+accel+inertia. Mac Mouse Fix (set to "use macOS settings") gives smooth+accel+precision and also fixes VS Code fast-scroll. Base scroll speed = scroll-triggers-per-rotation. SMOOTH_SCROLLING left y (harmless on macOS, helps high-res-aware hosts).

**USB HANG (resolved).** The sensor→behaviour→HID chain runs synchronously in the AS5600 poll thread (sensors.c non-ISR path runs the trigger in caller ctx), and each USB HID send blocks on a 30 ms semaphore (`app/src/usb_hid.c`). A fast spin fires the sensor ~200 Hz; ONE HID REPORT PER TICK sustained wedges USB HID TX and hangs the board (a host polling hiccup → 30 ms stall). Fix: `&enc_dispatch` COALESCES scroll line-deltas and emits at most once per scroll-min-interval-ms (12 ≈ 83/s, below the always-stable BLE rate), carrying the signed remainder so no rotation is lost; macOS rate-cap + app smoothing make it imperceptible. (The rate is what hung it, not the report path.)

Hardware-verified: volume + V/H scroll + tabs per profile, mode key cycles, display blanks at rest, no USB/BLE hang under sustained scrolling.

### Milestone 6 — Power + ZMK Studio polish.   [MOSTLY DONE — core HW-verified; remaining: user HW checks (BLE multi-host, battery % calibration, on-battery power draw)]

**Status-screen rework** [DONE, HW-verified]. Reworked the M4 layout: top battery row (unchanged), then `CT:` (connection type USB/BLE, from `zmk_endpoints_selected().transport`, subscribed to `zmk_endpoint_changed`) and `EM:` (encoder mode VOL/VSCR/HSCR/TABS, subscribed to the new `zmk_encoder_mode_changed` event) split labels (prefix left edge / value right edge, unscii_8), then a vertical profile column. The active profile is a full-height rounded cell with its layer name (unscii_8, ≤7 chars) inverted inside; inactive profiles are squeezed filled bars whose height scales to the live profile count. Cells are hand-drawn into a 1bpp `lv_canvas` (a sub-pixel symmetric rounded-rect rasterizer — crisp corners, no LVGL bevel) with direct bitmap writes + one `lv_obj_invalidate` per update (per-pixel `lv_canvas_set_px`/`fill_bg` invalidate per call → switch lag; avoid). Canvas gotcha: the I1 buffer has an 8-byte in-buffer palette that `lv_draw_buf_goto_xy` skips but `LV_CANVAS_BUF_SIZE` doesn't reserve — size the buffer with `+ palette_bytes` and take the bitmap base from `lv_draw_buf_goto_xy(buf,0,0)`.

**Flexible profile count + Studio add/delete** [DONE, HW-verified]. Keymap is now 5 blank profiles (`&none` except pos 3 `&enc_mode_next`, pos 12 `&profile_next`; P1 keeps `&studio_unlock` pos 8) + 2 `status = "reserved"` spare slots (P6, P7) → up to 7 (the screen's `MAX_PROFILES`). With reordering on (auto-selected — see below), `ZMK_KEYMAP_LAYERS_LEN` counts all 7; only the 5 okay layers start active, reserved ones are INVAL spares Studio's "add layer" fills. The dev variant (Studio RPC transport absent) excludes reserved layers entirely (stays at 5). `&profile_next` (`behavior_profile_next.c`) and `encoder_mode.c` size/wrap on the *live* valid-layer count, so cycling tracks adds/removes. `menuconfig ZMK_STUDIO` (in `macro_keyboard.conf`) selects `ZMK_STUDIO_RPC` for non-split boards → which selects BOTH `ZMK_KEYMAP_LAYER_REORDERING` and `ZMK_KEYMAP_SETTINGS_STORAGE`, in *both* variants. So Studio add/remove/rename of profiles persists in NVS.

**AS5600 activity-gating (power)** [DONE, HW-verify pending]. The poll thread now backs off from `poll-period-ms` (5 ms) to `CONFIG_AS5600_IDLE_POLL_PERIOD_MS` (100 ms) whenever `zmk_activity_get_state() != ZMK_ACTIVITY_ACTIVE` — cutting the ~200 wakeups/s that blocked the SoC's low-power idle while the (deadbanded, event-free) magnet sits still. The first notch after idle raises a sensor event → activity resets to ACTIVE → fast poll resumes next loop (self-correcting; worst-case first-notch latency = the idle period). The driver is its own `zephyr_library` subdir, so its `CMakeLists.txt` needs the `app/include` path added explicitly to reach `<zmk/activity.h>`.

**Encoder-mode persistence** [DONE, HW-verified]. `encoder_mode.c` persists the per-profile modes array to NVS under `encmode/modes` via a `SETTINGS_STATIC_HANDLER_DEFINE` (loaded by ZMK's `settings_load()` at boot) + a `settings_save_one` on each `encoder_mode_cycle()`. The save is dispatched **immediately** (`k_work_reschedule(..., K_NO_WAIT)` on the system wq, NOT the 60 s `ZMK_SETTINGS_SAVE_DEBOUNCE`) so a mode change survives an immediate power-off; rapid cycles in one wq tick still coalesce. Blob size is stable (compile-time `PROFILE_COUNT`); a length mismatch on load falls back to the firmware defaults.

**Status-screen cell geometry** [DONE, HW-verified]. Two refinements over the rework above: (1) every cell (active + inactive) uses one fixed `CELL_RADIUS` (5) for its round-off — the old radius = squeezed/2 only looked right at count 5, giving a bevel/too-small corner at other counts; (2) inactive cells are capped at `MAX_SQUEEZED_H` (10 px) and the column is NOT stretched to the bottom edge, so e.g. with 2 profiles the unselected cell is a 10 px pill with free space below rather than a giant bar. Inactive height only shrinks below 10 px (to the 5 px floor) when too many profiles exist to fit.

**USB crash — root cause: clipped I1 canvas image → LVGL RGB565 layer OOB** [RESOLVED, HW-verified]. Symptom: with USB connected the board went totally unresponsive (a few minutes at first; once `HW_STACK_PROTECTION`+reboot were added it became an immediate reboot-loop on USB), recovering the instant USB was unplugged; never on BLE. The one thing drawn ONLY on USB is the **battery charging icon** (the lightning bolt). Diagnosed on-screen, no debugger: (1) `FLT display queue r26` = a fault in ZMK's **display work queue**; the PC was garbage because r26 = `K_ERR_ARM_BUS_IMPRECISE_DATA_BUS` (imprecise → the stacked PC points at a *later* instruction). (2) Setting `ACTLR.DISDEFWBUF` (disable the Cortex-M write buffer) made faults **precise** → next capture `FLT display queue r19 pc=…` where r19 = `K_ERR_ARM_MEM_DATA_ACCESS` and the PC resolved to **`argb8888_image_blend` in `lv_draw_sw_blend_to_rgb565.c`** — LVGL blending through an **RGB565** path in a `LV_COLOR_DEPTH_1` build. Root cause: the bolt was an lv_canvas **image that was a clipped child of the bordered `batt_body` container and overlapped `batt_fill`**, so LVGL allocated an **RGB565 intermediate layer** whose buffer math is wrong for 1bpp, and the blend ran out of bounds → MPU data-access fault. `col_cbuf` never crashed because it's a **standalone, unclipped, direct child of the screen**. Fix: make the charge graphic exactly that — ONE interior-sized (16x9) standalone I1 canvas that is a direct child of `screen`, solid lit fill + bolt punched out, positioned in absolute coords in `battery_update`, with `batt_fill` HIDDEN while charging (no overlap). **Lessons: (a) on a 1bpp display, avoid making an lv_canvas/image a clipped child of a styled/bordered container or overlapping another obj — it triggers an RGB565 intermediate-layer path that's broken for `LV_COLOR_DEPTH_1`; keep decorative canvases as standalone direct children of the screen like `col_canvas`. (b) An imprecise bus fault (reason 26) has a useless PC — set `ACTLR.DISDEFWBUF` to force precise faults before trusting the captured PC. (c) Every I1 `lv_canvas` static buffer must add `LV_COLOR_INDEXED_PALETTE_SIZE(I1)*4` (=8 B, `I1_PALETTE_BYTES`) on top of `LV_CANVAS_BUF_SIZE` — a real latent bug fixed for both canvases, though not the crash cause here.**

Kept as resilience/robustness alongside the fix (permanent): `CONFIG_HW_STACK_PROTECTION=y` (MPU guard — was OFF despite `CONFIG_ARM_MPU=y`) and `module/src/fatal.c` overriding Zephyr's weak `k_sys_fatal_error_handler` (which spins forever) with `sys_reboot()` (needs `CONFIG_REBOOT=y`) so a fault recovers instead of hanging. The speculative stack bumps from the hunt (`USB_NRFX_WORK_QUEUE`, `MAIN`, AS5600) were **reverted** once the LVGL cause was found — they were never the issue. The USB-only timing was because on USB the board never deep-sleeps (`is_usb_power_present()` skips the sleep path) so it kept rendering the bolt, whereas on battery it deep-sleeps at the 15-min `ZMK_IDLE_SLEEP_TIMEOUT`.

*No-debugger fault-capture recipe (used here, since removed — reuse if a fault recurs).* A temporary `k_sys_fatal_error_handler` override recorded the faulting thread name (needs `CONFIG_THREAD_NAME=y`) + reason + PC into a `__noinit` struct — which survives the warm reboot AND a USB unplug while the LiPo keeps RAM powered — and `status_screen.c` showed `FLT <thread> r<reason> pc<hex>` on the next clean boot. Decoding: reason is `enum k_fatal_error_reason`, but ARM adds arch-specific codes at `K_ERR_ARCH_START = 16` (e.g. 19 = `K_ERR_ARM_MEM_DATA_ACCESS`, 26 = `K_ERR_ARM_BUS_IMPRECISE_DATA_BUS`); map the PC with `arm-zephyr-eabi-addr2line -e zmk.elf`. For an imprecise fault (26) the PC is useless until you set `ACTLR.DISDEFWBUF` (0xE000E008 bit 1) to force precise faults. All of that scaffolding (`fault_record.h`, the capture, the DISDEFWBUF init, `THREAD_NAME`) was stripped after the fix was confirmed.

**Studio active-layer indicator** [RESOLVED — not a firmware issue]. The keymap RPC `Notification` message (`zmk-studio-messages/keymap.proto`) carries only `unsaved_changes_status_changed`; there is no active-layer-changed notification in this ZMK Studio version, and the docs list no live active-layer indicator. So Studio cannot highlight the hardware-active layer — by design, not misconfig. The on-device OLED profile column already serves that role.

**Remaining (user HW verification only):**
- BLE multi-host: config supports 5 paired hosts (`BT_MAX_PAIRED=5`, `BT_MAX_CONN=5`). Host-switching keys (`&bt BT_SEL n`, `&bt BT_CLR`, `&out`) are NOT pre-bound (keymap is all-`&none` by request) — assign in Studio to test/switch across hosts.
- Battery divider: DTS scaling is correct for the measured hardware (330k Batt+→VD_ADC, 1M VD_ADC→GND → `output-ohms=1000000`, `full-ohms=1330000`, ×1.33). Remaining is calibrating the on-screen % against a multimeter reading of the pack.
