# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT

zephyr_library()
zephyr_library_sources(as5600.c)

# ZMK's public headers (zmk/activity.h, ...) live under app/include and are only
# added to the `app` target. This driver is its own zephyr_library in a subdir,
# so the parent module's include path doesn't reach it — add it here too (the
# driver gates its poll rate on zmk_activity_get_state()).
zephyr_library_include_directories(${ZEPHYR_BASE}/../app/include)
