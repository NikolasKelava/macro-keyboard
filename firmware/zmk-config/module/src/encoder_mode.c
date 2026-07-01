/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 * Per-profile encoder-mode state. See encoder_mode.h.
 */

#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include <zmk/keymap.h>

#include "encoder_mode.h"

LOG_MODULE_REGISTER(encoder_mode, CONFIG_ZMK_LOG_LEVEL);

ZMK_EVENT_IMPL(zmk_encoder_mode_changed);

/* One mode slot per keymap layer (profile). Sized to the compile-time layer
 * count so it scales with however many profiles exist. */
#define PROFILE_COUNT ZMK_KEYMAP_LAYERS_LEN

/* Default mode per profile; profiles beyond these zero-init to ENC_MODE_VOLUME
 * (value 0). Reach horizontal scroll by cycling &enc_mode_next. These defaults
 * apply on a fresh device; settings_load() (below) overwrites them with the
 * persisted values at boot if any were saved. */
static enum encoder_mode modes[PROFILE_COUNT] = {
    ENC_MODE_VOLUME,
    ENC_MODE_VSCROLL,
    ENC_MODE_TABS,
    ENC_MODE_VOLUME,
};

/* Persistence (NVS via the settings subsystem). The per-profile modes are saved
 * under "encmode/modes" as the raw array blob and reloaded at boot by ZMK's
 * settings_load() in main.c. The save is dispatched immediately (K_NO_WAIT) on
 * the system work queue rather than debounced: mode changes are deliberate and
 * infrequent, and the user wants a cycle to survive an immediate power-off, so
 * we persist as soon as possible (off the caller's stack — flash writes don't
 * belong in the behavior/event context). Rapid cycles within one work-queue
 * tick still coalesce into a single save of the final value. The blob size is
 * stable across Studio add/remove (PROFILE_COUNT is the compile-time layer
 * count, incl. reserved slots); a length mismatch on load (e.g. a firmware
 * rebuild that changed the layer count) is ignored, falling back to defaults. */
#if IS_ENABLED(CONFIG_SETTINGS)

static void encoder_mode_save_work_cb(struct k_work *work) {
    ARG_UNUSED(work);
    int ret = settings_save_one("encmode/modes", modes, sizeof(modes));
    if (ret) {
        LOG_WRN("Failed to persist encoder modes (err %d)", ret);
    }
}

static K_WORK_DELAYABLE_DEFINE(encoder_mode_save_work, encoder_mode_save_work_cb);

static void encoder_mode_save(void) {
    k_work_reschedule(&encoder_mode_save_work, K_NO_WAIT);
}

static int encoder_mode_settings_set(const char *name, size_t len, settings_read_cb read_cb,
                                     void *cb_arg) {
    if (settings_name_steq(name, "modes", NULL)) {
        if (len != sizeof(modes)) {
            LOG_WRN("Stored encoder modes size %u != expected %u; using defaults", (unsigned)len,
                    (unsigned)sizeof(modes));
            return -EINVAL;
        }
        int ret = read_cb(cb_arg, modes, sizeof(modes));
        if (ret <= 0) {
            LOG_WRN("Failed to read encoder modes from settings (err %d)", ret);
            return ret;
        }
    }
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(encoder_mode, "encmode", NULL, encoder_mode_settings_set, NULL, NULL);

#else

static void encoder_mode_save(void) {}

#endif /* IS_ENABLED(CONFIG_SETTINGS) */

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
    encoder_mode_save();
    raise_zmk_encoder_mode_changed((struct zmk_encoder_mode_changed){.mode = (uint8_t)modes[p]});
}
