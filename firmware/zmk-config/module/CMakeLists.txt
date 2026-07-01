# macro_keyboard custom firmware module
#
# Adds project-specific source files (custom behaviors, custom sensor
# drivers, custom display screen, etc.) into the ZMK build. Activated by
# passing -DZMK_EXTRA_MODULES=<path-to-this-dir> on the west build line.
#
# Each source is gated by Kconfig at compile time so that only the firmware
# pieces we currently need are linked in.

zephyr_library()

# ZMK's public headers (zmk/keymap.h, zmk/display.h, ...) live under
# app/include but are only added to the `app` target, not exported
# globally. Pull them in explicitly so this out-of-tree module can build
# against them.
zephyr_library_include_directories(${ZEPHYR_BASE}/../app/include)

# M6 — Reboot on fatal error instead of Zephyr's default spin-forever halt.
zephyr_library_sources(src/fatal.c)

# M3 — Profiles + profile-button cycle.
zephyr_library_sources(src/behavior_profile_next.c)

# M5 — Encoder mode switching (each behavior self-gates on its DT node):
#   encoder_mode.c            shared per-profile mode state
#   behavior_encoder_mode_next.c  &enc_mode_next key behavior (cycles mode)
#   behavior_encoder_dispatch.c   &enc_dispatch sensor behavior (acts on mode)
zephyr_library_sources(src/encoder_mode.c)
zephyr_library_sources(src/behavior_encoder_mode_next.c)
zephyr_library_sources(src/behavior_encoder_dispatch.c)

# M4 — Custom display status screen.
zephyr_library_sources_ifdef(CONFIG_ZMK_DISPLAY_STATUS_SCREEN_CUSTOM src/status_screen.c)

# M5 — AS5600 magnetic encoder (own zephyr_library() in its subdir).
add_subdirectory_ifdef(CONFIG_AS5600 drivers/sensor/as5600)
