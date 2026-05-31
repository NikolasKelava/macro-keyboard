/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 * Custom status screen for macro_keyboard's 128x64 SSD1306, rendered
 * in 90° (portrait) orientation: 64 px wide x 128 px tall.
 *
 *   +-------------+   <- top of the device once rotated to the right
 *   | [icn] NN%   |   battery row (Montserrat 12)
 *   |             |
 *   |   +-----+   |
 *   |   |  1  |   |   active profile = filled cell + inverted digit
 *   |   +-----+   |
 *   |   |  2  |   |   inactive       = rounded outline + plain digit
 *   |   +-----+   |
 *   |   |  3  |   |
 *   |   +-----+   |
 *   |   |  4  |   |
 *   |   +-----+   |
 *   +-------------+
 *
 * Rotation: LVGL v9's lv_display_set_rotation() only flips the logical
 * resolution; Zephyr's mono flush callback writes LVGL's logical coords
 * straight to the panel and does not transform pixels. We install
 * `rotated_flush_cb` over the top: it pre-rotates the LVGL pixel buffer
 * 270° CW (= 90° CCW) into a static scratch buffer and recomputes the
 * area into the panel's native 128x64 coordinate system, then hands it
 * to Zephyr's lvgl_flush_cb_mono which handles the SSD1306 page-major
 * conversion. Direction chosen by hardware test — the device is mounted
 * with what's physically the left edge of the panel at the user's top.
 *
 * Polarity: our DTS has no `inversion-on`, so the SSD1306 driver reports
 * PIXEL_FORMAT_MONO01 to LVGL. In that format `lv_color_white()` maps
 * to OFF and `lv_color_black()` to LIT. theme_mono is already calibrated
 * for this; we leave the screen bg/text to the theme and swap
 * white<->black only in our custom cell styles.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>

#include <lvgl.h>

#include <zmk/battery.h>
#include <zmk/display.h>
#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/keymap.h>
#include <zmk/usb.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define NUM_PROFILES   4
#define CELL_W         44
#define CELL_H         22
#define CELL_GAP        4
#define CELL_TOP_START 24    /* y of the first cell's top edge in 64x128 logical canvas */

/* Native panel orientation, before our 90° rotation. */
#define PANEL_W 128
#define PANEL_H 64

static lv_obj_t *battery_label;
static lv_obj_t *profile_cells[NUM_PROFILES];
static lv_style_t style_cell_default;
static lv_style_t style_cell_active;

/* ----------------------------------------------------- rotated flush callback
 *
 * LVGL renders into a buffer in the logical (rotated) orientation. We need
 * the data in the panel's native 128x64 orientation before Zephyr's mono
 * flush can push it out. The buffer LVGL hands us is at most one VDB worth
 * of 1bpp pixels (LV_Z_VDB_SIZE % of the display); 1100 bytes covers the
 * worst case (full 128*64/8 + 8-byte palette header + alignment slack).
 */

/* Defined in zephyr/modules/lvgl/lvgl_display_mono.c. */
extern void lvgl_flush_cb_mono(lv_display_t *display, const lv_area_t *area, uint8_t *px_map);

#define LVGL_PALETTE_HEADER_SIZE 8
#define ROTATED_BUF_SIZE        1100

static uint8_t rotated_buf[ROTATED_BUF_SIZE];

static void rotated_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    const int32_t w_log = area->x2 - area->x1 + 1;
    const int32_t h_log = area->y2 - area->y1 + 1;
    /* 270° CW (= 90° CCW) within the area: (lx, ly) -> (ly, w_log-1-lx).
     * Physical local width = h_log, height = w_log. */
    const int32_t w_phy = h_log;

    uint8_t *src = px_map + LVGL_PALETTE_HEADER_SIZE;
    uint8_t *dst = rotated_buf + LVGL_PALETTE_HEADER_SIZE;

    memset(dst, 0, sizeof(rotated_buf) - LVGL_PALETTE_HEADER_SIZE);

    /* CONFIG_LV_DRAW_BUF_STRIDE_ALIGN is 1, so row stride equals width in
     * bits. Densely packed, MSB-first. */
    for (int32_t ly = 0; ly < h_log; ly++) {
        const int32_t row_base = ly * w_log;
        for (int32_t lx = 0; lx < w_log; lx++) {
            const int32_t bit_l = row_base + lx;
            if ((src[bit_l >> 3] >> (7 - (bit_l & 7))) & 1) {
                const int32_t px = ly;
                const int32_t py = w_log - 1 - lx;
                const int32_t bit_p = py * w_phy + px;
                dst[bit_p >> 3] |= (uint8_t)(1u << (7 - (bit_p & 7)));
            }
        }
    }

    /* Same rotation applied to the area corners maps the logical area to a
     * physical-orientation area on the 128x64 panel. */
    const lv_area_t phys_area = {
        .x1 = area->y1,
        .x2 = area->y2,
        .y1 = PANEL_H - 1 - area->x2,
        .y2 = PANEL_H - 1 - area->x1,
    };
    lvgl_flush_cb_mono(disp, &phys_area, rotated_buf);
}

/* ------------------------------------------------------------------ battery */

struct battery_state {
    uint8_t level;
    bool usb_present;
};

static const char *battery_icon_for(uint8_t level) {
    if (level > 95) return LV_SYMBOL_BATTERY_FULL;
    if (level > 65) return LV_SYMBOL_BATTERY_3;
    if (level > 35) return LV_SYMBOL_BATTERY_2;
    if (level > 5)  return LV_SYMBOL_BATTERY_1;
    return LV_SYMBOL_BATTERY_EMPTY;
}

static void battery_update(struct battery_state state) {
    char text[16];
    if (state.usb_present) {
        snprintf(text, sizeof(text), LV_SYMBOL_CHARGE " %s %u%%",
                 battery_icon_for(state.level), state.level);
    } else {
        snprintf(text, sizeof(text), "%s %u%%",
                 battery_icon_for(state.level), state.level);
    }
    lv_label_set_text(battery_label, text);
}

static struct battery_state battery_get_state(const zmk_event_t *eh) {
    const struct zmk_battery_state_changed *ev = as_zmk_battery_state_changed(eh);
    return (struct battery_state){
        .level = (ev != NULL) ? ev->state_of_charge : zmk_battery_state_of_charge(),
        .usb_present = zmk_usb_is_powered(),
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_battery, struct battery_state,
                            battery_update, battery_get_state)
ZMK_SUBSCRIPTION(widget_battery, zmk_battery_state_changed);
ZMK_SUBSCRIPTION(widget_battery, zmk_usb_conn_state_changed);

/* ----------------------------------------------------------------- profiles */

struct profile_state {
    uint8_t active;
};

static void profile_update(struct profile_state state) {
    for (uint8_t i = 0; i < NUM_PROFILES; i++) {
        if (i == state.active) {
            lv_obj_add_state(profile_cells[i], LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(profile_cells[i], LV_STATE_CHECKED);
        }
    }
}

static struct profile_state profile_get_state(const zmk_event_t *eh) {
    zmk_keymap_layer_index_t idx = zmk_keymap_highest_layer_active();
    return (struct profile_state){
        .active = (idx < NUM_PROFILES) ? (uint8_t)idx : 0,
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_profile, struct profile_state,
                            profile_update, profile_get_state)
ZMK_SUBSCRIPTION(widget_profile, zmk_layer_state_changed);

/* ------------------------------------------------------------------- screen */

lv_obj_t *zmk_display_status_screen(void) {
    /* Install our rotation BEFORE creating screen objects so the first
     * render goes through the rotated flush path. */
    lv_display_t *disp = lv_display_get_default();
    lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_270);
    lv_display_set_flush_cb(disp, rotated_flush_cb);

    lv_obj_t *screen = lv_obj_create(NULL);

    /* Battery row: Montserrat 12 (~14 px line) at the top-right of the
     * rotated canvas — that's the top edge once the device is held upright. */
    battery_label = lv_label_create(screen);
    lv_obj_set_style_text_font(battery_label, &lv_font_montserrat_12, 0);
    lv_label_set_text(battery_label, "");
    lv_obj_align(battery_label, LV_ALIGN_TOP_RIGHT, -2, 2);

    lv_style_init(&style_cell_default);
    lv_style_set_radius(&style_cell_default, 3);
    lv_style_set_border_width(&style_cell_default, 1);
    lv_style_set_border_color(&style_cell_default, lv_color_black());
    lv_style_set_border_opa(&style_cell_default, LV_OPA_COVER);
    lv_style_set_bg_opa(&style_cell_default, LV_OPA_TRANSP);
    lv_style_set_pad_all(&style_cell_default, 0);

    lv_style_init(&style_cell_active);
    lv_style_set_bg_opa(&style_cell_active, LV_OPA_COVER);
    lv_style_set_bg_color(&style_cell_active, lv_color_black());
    lv_style_set_border_opa(&style_cell_active, LV_OPA_TRANSP);
    lv_style_set_text_color(&style_cell_active, lv_color_white());

    static const char *const labels[NUM_PROFILES] = {"1", "2", "3", "4"};
    const int16_t stride = CELL_H + CELL_GAP;
    for (uint8_t i = 0; i < NUM_PROFILES; i++) {
        lv_obj_t *cell = lv_obj_create(screen);
        lv_obj_remove_style_all(cell);
        lv_obj_add_style(cell, &style_cell_default, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_add_style(cell, &style_cell_active, LV_PART_MAIN | LV_STATE_CHECKED);
        lv_obj_set_size(cell, CELL_W, CELL_H);
        lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(cell, LV_ALIGN_TOP_MID, 0, CELL_TOP_START + i * stride);

        lv_obj_t *digit = lv_label_create(cell);
        lv_label_set_text(digit, labels[i]);
        lv_obj_center(digit);
        profile_cells[i] = cell;
    }

    widget_battery_init();
    widget_profile_init();

    return screen;
}
