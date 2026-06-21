/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 * Custom ZMK sensor behavior: &enc_dispatch.
 *
 * Bound as the sensor-binding on every profile layer. Instead of a fixed
 * action, it reads the current profile's encoder mode (see encoder_mode.c) at
 * the moment of each tick and emits the matching action:
 *
 *   ENC_MODE_VOLUME  -> &kp C_VOL_UP / C_VOL_DN       (queued key presses)
 *   ENC_MODE_VSCROLL -> INPUT_REL_WHEEL  report        (up/down)
 *   ENC_MODE_HSCROLL -> INPUT_REL_HWHEEL report        (right/left)
 *   ENC_MODE_TABS    -> &kp LC(TAB) / LC(LS(TAB))      (next/prev browser tab)
 *
 * Per-mode resolution. This accumulates raw rotation in micro-degrees and
 * quantises it with the *current mode's* own triggers-per-rotation, so each
 * mode picks how many ticks a full turn produces:
 *   - volume + tabs use a COARSE value (a small turn = one step), and
 *   - the scroll modes use a finer value.
 * The sub-tick remainder is carried, so nothing is lost and switching mode
 * mid-turn is seamless.
 *
 * --- Scroll model (clean linear stream; the host owns the feel) -------------
 * Long investigation conclusion: on macOS the scroll *feel* (smoothing +
 * acceleration) is owned by the host. macOS velocity-caps the report rate
 * (more reports/s does not scroll faster) and either applies its own bad accel
 * curve or, via an app, normalises magnitude. The good outcome is a smooth-
 * scroll app (Mac Mouse Fix / Mos) doing pixel smoothing + tunable
 * acceleration. So the firmware deliberately does NO acceleration: it emits a
 * clean LINEAR stream of wheel line-deltas (d_scroll = ticks * scroll-units)
 * and lets the app shape it. Base speed is set by scroll-triggers-per-rotation
 * (lines per revolution). One report per sensor event keeps the rate bounded.
 *
 * CW (positive rotation) maps to volume-up / scroll-up / scroll-right / next-tab.
 */

#define DT_DRV_COMPAT zmk_behavior_encoder_dispatch

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <dt-bindings/zmk/keys.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/behavior_queue.h>
#include <zmk/keymap.h>
#include <zmk/sensors.h>
#include <zmk/virtual_key_position.h>

#include "encoder_mode.h"

#if IS_ENABLED(CONFIG_ZMK_POINTING)
#include <zephyr/input/input.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#endif

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

/* Rotation is accumulated in micro-degrees so the modes can use very different
 * resolutions without rounding drift. */
#define MICRODEG_PER_DEG 1000000
#define MICRODEG_PER_REV 360000000

#if IS_ENABLED(CONFIG_ZMK_POINTING)
/* The &msc node's device doubles as the input device its listener consumes. */
static const struct device *const scroll_dev = DEVICE_DT_GET(DT_NODELABEL(msc));
#endif

struct enc_dispatch_config {
    int tap_ms;
    int scroll_units;          /* wheel line-delta per scroll tick (keep small)   */
    int volume_tpr;            /* triggers per rotation in volume mode (coarse)   */
    int tab_tpr;               /* triggers per rotation in tab mode (coarse)      */
    int scroll_tpr;            /* triggers per rotation in scroll modes           */
    int scroll_max_value;      /* safety clamp on the per-report wheel magnitude  */
    int scroll_min_interval_ms; /* min gap between scroll HID reports (coalesce)  */
};

struct enc_dispatch_data {
    int32_t accum_microdeg[ZMK_KEYMAP_SENSORS_LEN][ZMK_KEYMAP_LAYERS_LEN];
    int ticks[ZMK_KEYMAP_SENSORS_LEN][ZMK_KEYMAP_LAYERS_LEN];
#if IS_ENABLED(CONFIG_ZMK_POINTING)
    /* Scroll report coalescing/rate-limit state (per sensor). */
    int32_t scroll_pending[ZMK_KEYMAP_SENSORS_LEN]; /* signed line-deltas not yet sent */
    uint16_t scroll_axis[ZMK_KEYMAP_SENSORS_LEN];   /* INPUT_REL_WHEEL / HWHEEL         */
    uint32_t last_scroll_ms[ZMK_KEYMAP_SENSORS_LEN]; /* last emit time                  */
#endif
};

/* Triggers-per-rotation for the active mode: coarse for the discrete key modes
 * (volume, tabs), finer for the scroll modes. */
static int enc_dispatch_tpr(const struct enc_dispatch_config *cfg, enum encoder_mode m) {
    switch (m) {
    case ENC_MODE_VOLUME:
        return cfg->volume_tpr;
    case ENC_MODE_TABS:
        return cfg->tab_tpr;
    default:
        return cfg->scroll_tpr;
    }
}

static int enc_dispatch_accept_data(struct zmk_behavior_binding *binding,
                                    struct zmk_behavior_binding_event event,
                                    const struct zmk_sensor_config *sensor_config,
                                    size_t channel_data_size,
                                    const struct zmk_sensor_channel_data *channel_data) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    const struct enc_dispatch_config *cfg = dev->config;
    struct enc_dispatch_data *data = dev->data;

    ARG_UNUSED(sensor_config); /* resolution is per-mode (below), not the node's */
    ARG_UNUSED(channel_data_size);

    const struct sensor_value value = channel_data[0].value;
    const int sensor_index = ZMK_SENSOR_POSITION_FROM_VIRTUAL_KEY_POSITION(event.position);

    /* Accumulate rotation in micro-degrees. The AS5600 driver reports
     * whole+fractional degrees on SENSOR_CHAN_ROTATION. */
    int32_t accum = data->accum_microdeg[sensor_index][event.layer] +
                    value.val1 * MICRODEG_PER_DEG + value.val2;

    /* Quantise with the current mode's triggers-per-rotation; carry the
     * sub-tick remainder. */
    const int tpr = enc_dispatch_tpr(cfg, encoder_mode_get());
    const int32_t trigger_microdeg = MICRODEG_PER_REV / tpr;
    const int ticks = accum / trigger_microdeg;

    data->accum_microdeg[sensor_index][event.layer] = accum - ticks * trigger_microdeg;
    data->ticks[sensor_index][event.layer] = ticks;
    return 0;
}

static int enc_dispatch_process(struct zmk_behavior_binding *binding,
                                struct zmk_behavior_binding_event event,
                                enum behavior_sensor_binding_process_mode mode) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    const struct enc_dispatch_config *cfg = dev->config;
    struct enc_dispatch_data *data = dev->data;

    const int sensor_index = ZMK_SENSOR_POSITION_FROM_VIRTUAL_KEY_POSITION(event.position);

    if (mode != BEHAVIOR_SENSOR_BINDING_PROCESS_MODE_TRIGGER) {
        data->ticks[sensor_index][event.layer] = 0;
        return ZMK_BEHAVIOR_TRANSPARENT;
    }

    int ticks = data->ticks[sensor_index][event.layer];
    if (ticks == 0) {
        return ZMK_BEHAVIOR_TRANSPARENT;
    }

    const bool cw = ticks > 0;
    const int count = cw ? ticks : -ticks;
    const enum encoder_mode m = encoder_mode_get();

#if IS_ENABLED(CONFIG_ZMK_POINTING)
    if (m == ENC_MODE_VSCROLL || m == ENC_MODE_HSCROLL) {
        const uint16_t code = (m == ENC_MODE_VSCROLL) ? INPUT_REL_WHEEL : INPUT_REL_HWHEEL;

        /* Coalesce + rate-limit. A fast spin fires the sensor up to ~200 Hz;
         * emitting a USB HID report for every tick, sustained over a long scroll,
         * wedges the HID TX path (each send blocks the encoder thread on a
         * semaphore) and hangs the board. So accumulate the signed line-delta and
         * emit at most once per scroll-min-interval-ms, carrying the remainder.
         * macOS velocity-caps scrolling and the host app smooths between reports,
         * so the lower report rate is imperceptible but no rotation is lost. */
        if (data->scroll_axis[sensor_index] != code) {
            data->scroll_pending[sensor_index] = 0;
            data->scroll_axis[sensor_index] = code;
        }
        data->scroll_pending[sensor_index] += cw ? count : -count;

        const uint32_t now = k_uptime_get_32();
        if ((now - data->last_scroll_ms[sensor_index]) >= (uint32_t)cfg->scroll_min_interval_ms) {
            int32_t mag = data->scroll_pending[sensor_index] * cfg->scroll_units;
            if (mag > cfg->scroll_max_value) {
                mag = cfg->scroll_max_value; /* safety clamp */
            } else if (mag < -cfg->scroll_max_value) {
                mag = -cfg->scroll_max_value;
            }
            if (mag != 0 && device_is_ready(scroll_dev)) {
                input_report_rel(scroll_dev, code, mag, true, K_NO_WAIT);
            }
            data->scroll_pending[sensor_index] = 0;
            data->last_scroll_ms[sensor_index] = now;
        }
        return ZMK_BEHAVIOR_OPAQUE;
    }
#endif

    /* Discrete key modes: tabs (Ctrl+Tab / Ctrl+Shift+Tab) and volume
     * (and the fallback when pointing is disabled). Queue one press/release
     * pair per tick. */
    const uint32_t keycode = (m == ENC_MODE_TABS) ? (cw ? LC(TAB) : LC(LS(TAB)))
                                                  : (cw ? C_VOL_UP : C_VOL_DN);
    struct zmk_behavior_binding action = {
        .behavior_dev = DEVICE_DT_NAME(DT_NODELABEL(kp)),
        .param1 = keycode,
    };
    for (int i = 0; i < count; i++) {
        zmk_behavior_queue_add(&event, action, true, cfg->tap_ms);
        zmk_behavior_queue_add(&event, action, false, 0);
    }

    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api enc_dispatch_driver_api = {
    .sensor_binding_accept_data = enc_dispatch_accept_data,
    .sensor_binding_process = enc_dispatch_process,
};

#define ENC_DISPATCH_INST(n)                                                                        \
    static struct enc_dispatch_data enc_dispatch_data_##n = {};                                     \
    static const struct enc_dispatch_config enc_dispatch_config_##n = {                             \
        .tap_ms = DT_INST_PROP(n, tap_ms),                                                          \
        .scroll_units = DT_INST_PROP(n, scroll_units),                                              \
        .volume_tpr = DT_INST_PROP(n, volume_triggers_per_rotation),                                \
        .tab_tpr = DT_INST_PROP(n, tab_triggers_per_rotation),                                      \
        .scroll_tpr = DT_INST_PROP(n, scroll_triggers_per_rotation),                                \
        .scroll_max_value = DT_INST_PROP(n, scroll_max_value),                                      \
        .scroll_min_interval_ms = DT_INST_PROP(n, scroll_min_interval_ms),                          \
    };                                                                                              \
    BEHAVIOR_DT_INST_DEFINE(n, NULL, NULL, &enc_dispatch_data_##n, &enc_dispatch_config_##n,        \
                            POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                       \
                            &enc_dispatch_driver_api);

DT_INST_FOREACH_STATUS_OKAY(ENC_DISPATCH_INST)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
