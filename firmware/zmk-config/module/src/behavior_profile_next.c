/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 * Custom ZMK behavior: &profile_next.
 *
 * On press: read the current highest-active layer, increment with wrap,
 * then call zmk_keymap_layer_to() to make that the new active layer.
 *
 * Bound on every layer's profile-button position so cycling is consistent
 * regardless of which layer the user is currently on. Replaces the per-
 * layer `&to <N>` chain, which was unreliable when chained across layers.
 */

#define DT_DRV_COMPAT zmk_behavior_profile_next

#include <zephyr/device.h>
#include <zephyr/logging/log.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/keymap.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#define PROFILE_COUNT 4

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    zmk_keymap_layer_index_t cur = zmk_keymap_highest_layer_active();
    zmk_keymap_layer_index_t next_idx = (cur + 1) % PROFILE_COUNT;
    zmk_keymap_layer_id_t next_id = zmk_keymap_layer_index_to_id(next_idx);

    LOG_DBG("profile_next: layer %d -> %d", cur, next_idx);
    zmk_keymap_layer_to(next_id, false);
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_profile_next_driver_api = {
    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .get_parameter_metadata = zmk_behavior_get_empty_param_metadata,
#endif
};

BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, POST_KERNEL,
                        CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
                        &behavior_profile_next_driver_api);

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
