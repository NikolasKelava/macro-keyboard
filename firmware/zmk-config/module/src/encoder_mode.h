/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 * Runtime encoder-mode state, shared by the &enc_dispatch sensor behavior
 * (which acts on the mode) and the &enc_mode_next key behavior (which cycles
 * it). The mode is tracked per profile (= per keymap layer 0..3): each profile
 * remembers its own encoder mode, with firmware-defined defaults. ZMK Studio
 * cannot edit encoder bindings in this ZMK version, so this is how "encoder
 * mode per profile" is realised. State is RAM-only and resets to defaults on
 * reboot.
 */

#pragma once

enum encoder_mode {
    ENC_MODE_VOLUME = 0, /* CW = volume up      / CCW = volume down  */
    ENC_MODE_VSCROLL,    /* CW = scroll up      / CCW = scroll down  */
    ENC_MODE_HSCROLL,    /* CW = scroll right   / CCW = scroll left  */
    ENC_MODE_COUNT,
};

/* Mode of the currently-active profile. */
enum encoder_mode encoder_mode_get(void);

/* Advance the currently-active profile's mode by one (wraps). */
void encoder_mode_cycle(void);
