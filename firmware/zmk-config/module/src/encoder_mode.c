/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 * Per-profile encoder-mode state. See encoder_mode.h.
 */

#include <zmk/keymap.h>

#include "encoder_mode.h"

#define PROFILE_COUNT 4

/* Default mode per profile. Mirrors the original M5 plan:
 * P1 volume, P2 vertical scroll, P3 horizontal scroll, P4 volume. */
static enum encoder_mode modes[PROFILE_COUNT] = {
    ENC_MODE_VOLUME,
    ENC_MODE_VSCROLL,
    ENC_MODE_HSCROLL,
    ENC_MODE_VOLUME,
};

/* Profile == highest active keymap layer (same source &profile_next advances),
 * clamped into range for safety. */
static int current_profile(void) {
    int p = zmk_keymap_highest_layer_active();
    if (p < 0 || p >= PROFILE_COUNT) {
        p = 0;
    }
    return p;
}

enum encoder_mode encoder_mode_get(void) {
    return modes[current_profile()];
}

void encoder_mode_cycle(void) {
    int p = current_profile();
    modes[p] = (modes[p] + 1) % ENC_MODE_COUNT;
}
