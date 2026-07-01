/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 * Runtime encoder-mode state, shared by the &enc_dispatch sensor behavior
 * (which acts on the mode) and the &enc_mode_next key behavior (which cycles
 * it). The mode is tracked per profile (= per keymap layer): each profile
 * remembers its own encoder mode, with firmware-defined defaults. ZMK Studio
 * cannot edit encoder bindings in this ZMK version, so this is how "encoder
 * mode per profile" is realised. State persists across reboot via the settings
 * subsystem (NVS, key "encmode/modes"); a fresh device uses the defaults.
 */

#pragma once

#include <zephyr/kernel.h>

#include <zmk/event_manager.h>

enum encoder_mode {
    ENC_MODE_VOLUME = 0, /* CW = volume up      / CCW = volume down       */
    ENC_MODE_VSCROLL,    /* CW = scroll up      / CCW = scroll down        */
    ENC_MODE_HSCROLL,    /* CW = scroll right   / CCW = scroll left        */
    ENC_MODE_TABS,       /* CW = next tab       / CCW = previous tab       */
    ENC_MODE_COUNT,
};

/* Mode of the currently-active profile. */
enum encoder_mode encoder_mode_get(void);

/* Advance the currently-active profile's mode by one (wraps). Raises
 * zmk_encoder_mode_changed so the status screen's encoder-mode indicator can
 * refresh. */
void encoder_mode_cycle(void);

/* Raised whenever the active profile's encoder mode changes (via
 * &enc_mode_next). The status screen subscribes to this to update its on-screen
 * mode indicator; it also re-reads on layer change, since switching profile can
 * change the displayed mode. */
struct zmk_encoder_mode_changed {
    uint8_t mode;
};

ZMK_EVENT_DECLARE(zmk_encoder_mode_changed);
