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
 *   ENC_MODE_VOLUME  -> &kp C_VOL_UP / C_VOL_DN  (queued key presses)
 *   ENC_MODE_VSCROLL -> discrete INPUT_REL_WHEEL  events (up/down)
 *   ENC_MODE_HSCROLL -> discrete INPUT_REL_HWHEEL events (right/left)
 *
 * Per-mode resolution. Unlike a stock sensor-rotate behaviour (which takes a
 * single triggers-per-rotation from the keymap-sensors node), this accumulates
 * raw rotation in micro-degrees and quantises it with the *current mode's* own
 * triggers-per-rotation: scroll gets a high value (fine, smooth, makes use of
 * the encoder's resolution + high-res scrolling), volume a low one (coarse, so
 * a small turn is one volume step). Both come from DT props on the &enc_dispatch
 * node. The sub-tick remainder is carried, so nothing is lost and switching
 * mode mid-turn is seamless.
 *
 * Scroll deliberately does NOT go through &msc (zmk,behavior-input-two-axis):
 * that behaviour is a velocity x time model meant for *held* mouse keys — a
 * brief per-tick press emits ~speed*period/1000 = ~0 counts, so an encoder
 * tick produces no scroll. Instead we report discrete wheel events straight to
 * the &msc input device (the same device its input-listener consumes), so each
 * tick is one crisp scroll step, like a notched wheel. High-res scrolling
 * (CONFIG_ZMK_POINTING_SMOOTH_SCROLLING) still applies in the HID layer.
 *
 * CW (positive rotation) maps to volume-up / scroll-up / scroll-right.
 */

#define DT_DRV_COMPAT zmk_behavior_encoder_dispatch

#include <zephyr/device.h>
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

/* Rotation is accumulated in micro-degrees so the two modes can use very
 * different resolutions without rounding drift. */
#define MICRODEG_PER_DEG 1000000
#define MICRODEG_PER_REV 360000000

#if IS_ENABLED(CONFIG_ZMK_POINTING)
/* The &msc node's device doubles as the input device its listener consumes. */
static const struct device *const scroll_dev = DEVICE_DT_GET(DT_NODELABEL(msc));
#endif

struct enc_dispatch_config {
    int tap_ms;
    int scroll_units;
    int volume_tpr; /* triggers per rotation in volume mode  */
    int scroll_tpr; /* triggers per rotation in scroll modes */
};

struct enc_dispatch_data {
    int32_t accum_microdeg[ZMK_KEYMAP_SENSORS_LEN][ZMK_KEYMAP_LAYERS_LEN];
    int ticks[ZMK_KEYMAP_SENSORS_LEN][ZMK_KEYMAP_LAYERS_LEN];
};

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
     * whole+fractional degrees on SENSOR_CHAN_ROTATION, always with val1 != 0
     * when it reports, so no legacy val2-as-tick-count handling is needed. */
    int32_t accum = data->accum_microdeg[sensor_index][event.layer] +
                    value.val1 * MICRODEG_PER_DEG + value.val2;

    /* Quantise with the current mode's triggers-per-rotation; carry the
     * sub-tick remainder. */
    const int tpr = (encoder_mode_get() == ENC_MODE_VOLUME) ? cfg->volume_tpr : cfg->scroll_tpr;
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
        const int32_t value = (cw ? 1 : -1) * count * cfg->scroll_units;

        /* One mouse report per sensor event. (An earlier attempt to emit one
         * report *per tick* to fight host scroll acceleration flooded the USB
         * HID TX path and hung the board on macOS — and didn't help, since the
         * acceleration is applied host-side regardless.) */
        if (device_is_ready(scroll_dev)) {
            input_report_rel(scroll_dev, code, value, true, K_NO_WAIT);
        }
        return ZMK_BEHAVIOR_OPAQUE;
    }
#endif

    /* Volume (and fallback when pointing is disabled): queue &kp presses. */
    struct zmk_behavior_binding action = {
        .behavior_dev = DEVICE_DT_NAME(DT_NODELABEL(kp)),
        .param1 = cw ? C_VOL_UP : C_VOL_DN,
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
        .scroll_tpr = DT_INST_PROP(n, scroll_triggers_per_rotation),                                \
    };                                                                                              \
    BEHAVIOR_DT_INST_DEFINE(n, NULL, NULL, &enc_dispatch_data_##n, &enc_dispatch_config_##n,        \
                            POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                       \
                            &enc_dispatch_driver_api);

DT_INST_FOREACH_STATUS_OKAY(ENC_DISPATCH_INST)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
