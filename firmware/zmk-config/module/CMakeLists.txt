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

# M3 — Profiles + profile-button cycle.
zephyr_library_sources(src/behavior_profile_next.c)

# M4 — Custom display status screen.
zephyr_library_sources_ifdef(CONFIG_ZMK_DISPLAY_STATUS_SCREEN_CUSTOM src/status_screen.c)
