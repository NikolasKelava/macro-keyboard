/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 * Custom status screen for macro_keyboard's 128x64 SSD1306.
 *
 * MINIMAL DIAGNOSTIC VERSION: just a single static label, no battery widget,
 * no event listener. If this renders + the board enumerates, we'll add the
 * pieces back one at a time. If this still hangs, the problem is structural
 * (not in our screen logic).
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <lvgl.h>

#include <zmk/display.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

lv_obj_t *zmk_display_status_screen(void) {
    lv_obj_t *screen = lv_obj_create(NULL);

    lv_obj_t *label = lv_label_create(screen);
    lv_label_set_text(label, "macro_keyboard");
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);

    return screen;
}
