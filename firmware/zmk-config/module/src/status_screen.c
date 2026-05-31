/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 * Custom status screen for macro_keyboard's 128x64 SSD1306, rendered
 * in 90° (portrait) orientation: 64 px wide x 128 px tall.
 *
 *   +-------------+   <- top of the device once rotated to the right
 *   |  [==] NN%   |   battery row, centred (custom icon + UNSCII 8)
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
 *
 * Crisp battery row: LVGL renders 4bpp anti-aliased glyphs (Montserrat,
 * and the FontAwesome battery/charge LV_SYMBOL icons baked into it) by
 * thresholding each pixel's luminance at 127 onto the I1 panel — the soft
 * edges land near that threshold and produce bulky, fuzzy glyphs with
 * stray lit pixels. So the battery row avoids fonts/symbols entirely:
 *   - percentage uses lv_font_unscii_8 (a true 1bpp bitmap font, 8 px,
 *     no anti-aliasing) so the digits are pixel-exact;
 *   - the battery is drawn from full-opacity axis-aligned rectangles
 *     (body outline + terminal nub + a level-proportional fill); these
 *     are never anti-aliased, so they stay crisp;
 *   - while charging the fill is replaced by a small lightning bolt drawn
 *     into a 1bpp lv_canvas (hand-set pixels, also crisp).
 * The whole row is re-centred on the canvas on every battery update.
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
/* Cells: 6 px margin on the left/right (CELL_W = 64 - 2*6) and the same 6 px
 * below the last cell (128 - (CELL_TOP_START + 4*H + 3*GAP) = 6), so the side
 * and bottom gaps to the screen edge match. They start just under the battery
 * row. */
#define CELL_W         52
#define CELL_H         22
#define CELL_GAP        6
#define CELL_TOP_START 16    /* y of the first cell's top edge in 64x128 logical canvas */
#define CELL_RADIUS     3

/* Native panel orientation, before our 90° rotation. */
#define PANEL_W 128
#define PANEL_H 64

/* Battery row geometry (logical 64-wide canvas). The body has a 1 px lit
 * border and no inner padding, so its content (interior) area is
 * BODY-2*border on each axis: 16 x 9.
 *   - discharging: a level bar inset 1 px inside the interior (14 x 7),
 *     width proportional to charge;
 *   - charging:    the interior is filled solid (16 x 9) and a bolt-shaped
 *     OFF cut-out (BOLT) is overlaid, spanning the full interior height. */
#define BATT_BODY_W    18
#define BATT_BODY_H    11
#define BATT_NUB_W      2
#define BATT_NUB_H      5
#define BATT_GAP        3    /* px between the nub and the percentage label */
#define BATT_ROW_Y      2    /* top margin of the battery row */
#define BATT_TEXT_H     8    /* lv_font_unscii_8 glyph/line height */
#define BATT_INNER_W   16    /* interior width  (BODY_W - 2*border) */
#define BATT_INNER_H    9    /* interior height (BODY_H - 2*border) */
#define BATT_FILL_W    14    /* level-bar max width  (interior - 2, 1 px inset) */
#define BATT_FILL_H     7    /* level-bar height     (interior - 2, 1 px inset) */

/* Charge bolt: spans the full interior height; drawn as an OFF cut-out over
 * the solid fill. */
#define BOLT_W 7
#define BOLT_H 9

static lv_obj_t *battery_label;
static lv_obj_t *batt_body;          /* outlined battery body */
static lv_obj_t *batt_nub;           /* terminal nub on the right */
static lv_obj_t *batt_fill;          /* level-proportional fill (hidden while charging) */
static lv_obj_t *batt_bolt;          /* charge bolt canvas (hidden unless charging) */
static lv_obj_t *profile_cells[NUM_PROFILES];
static lv_style_t style_cell_default;
static lv_style_t style_cell_active;
static lv_style_t style_batt_body;   /* lit 1 px outline, transparent fill */
static lv_style_t style_batt_lit;    /* solid lit fill, no border (nub + fill) */

/* 7x9 lightning bolt. Bit (BOLT_W-1) is the leftmost column. A set bit is a
 * bolt pixel, drawn as an OFF cut-out over the solid (lit) charging fill. */
static const uint8_t bolt_rows[BOLT_H] = {
    0x06, /* ....XX. */
    0x0C, /* ...XX.. */
    0x18, /* ..XX... */
    0x3E, /* .XXXXX. */
    0x7C, /* XXXXX.. */
    0x0C, /* ...XX.. */
    0x18, /* ..XX... */
    0x30, /* .XX.... */
    0x60, /* XX..... */
};

static uint8_t bolt_cbuf[LV_CANVAS_BUF_SIZE(BOLT_W, BOLT_H, 1, LV_DRAW_BUF_STRIDE_ALIGN)];

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

static void battery_update(struct battery_state state) {
    char text[8];
    snprintf(text, sizeof(text), "%u%%", state.level);
    lv_label_set_text(battery_label, text);

    /* Centre [body][nub] gap [label] across the canvas content width. */
    lv_point_t sz;
    lv_text_get_size(&sz, text, &lv_font_unscii_8, 0, 0, LV_COORD_MAX, 0);
    const int32_t group_w = BATT_BODY_W + BATT_NUB_W + BATT_GAP + sz.x;
    const int32_t cw = lv_obj_get_content_width(lv_obj_get_parent(batt_body));
    int32_t x0 = (cw - group_w) / 2;
    if (x0 < 0) {
        x0 = 0;
    }

    lv_obj_align(batt_body, LV_ALIGN_TOP_LEFT, x0, BATT_ROW_Y);
    lv_obj_align(batt_nub, LV_ALIGN_TOP_LEFT, x0 + BATT_BODY_W,
                 BATT_ROW_Y + (BATT_BODY_H - BATT_NUB_H) / 2);
    lv_obj_align(battery_label, LV_ALIGN_TOP_LEFT,
                 x0 + BATT_BODY_W + BATT_NUB_W + BATT_GAP,
                 BATT_ROW_Y + (BATT_BODY_H - BATT_TEXT_H) / 2);

    if (state.usb_present) {
        /* Charging: fill the interior solid and overlay the bolt cut-out. */
        lv_obj_align(batt_fill, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_set_size(batt_fill, BATT_INNER_W, BATT_INNER_H);
        lv_obj_clear_flag(batt_fill, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(batt_bolt, LV_OBJ_FLAG_HIDDEN);
    } else {
        /* Discharging: level bar inset 1 px inside the interior. */
        lv_obj_add_flag(batt_bolt, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(batt_fill, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(batt_fill, LV_ALIGN_TOP_LEFT, 1, 1);
        int32_t fw = (int32_t)state.level * BATT_FILL_W / 100;
        if (fw < 1 && state.level > 0) {
            fw = 1;
        }
        lv_obj_set_size(batt_fill, fw, BATT_FILL_H);
    }
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

    /* ---- battery row: custom crisp icon + UNSCII 8 percentage ----
     * All three pieces are positioned (and re-centred) in battery_update;
     * here we only create them. */

    /* body: 1 px lit outline, transparent interior, no padding so children are
     * positioned directly against the inside of the border. */
    lv_style_init(&style_batt_body);
    lv_style_set_radius(&style_batt_body, 0);
    lv_style_set_border_width(&style_batt_body, 1);
    lv_style_set_border_color(&style_batt_body, lv_color_black()); /* lit on MONO01 */
    lv_style_set_border_opa(&style_batt_body, LV_OPA_COVER);
    lv_style_set_bg_opa(&style_batt_body, LV_OPA_TRANSP);
    lv_style_set_pad_all(&style_batt_body, 0);

    /* solid lit rectangles: nub + level fill. */
    lv_style_init(&style_batt_lit);
    lv_style_set_radius(&style_batt_lit, 0);
    lv_style_set_border_width(&style_batt_lit, 0);
    lv_style_set_bg_opa(&style_batt_lit, LV_OPA_COVER);
    lv_style_set_bg_color(&style_batt_lit, lv_color_black());      /* lit on MONO01 */
    lv_style_set_pad_all(&style_batt_lit, 0);

    batt_body = lv_obj_create(screen);
    lv_obj_remove_style_all(batt_body);
    lv_obj_add_style(batt_body, &style_batt_body, 0);
    lv_obj_set_size(batt_body, BATT_BODY_W, BATT_BODY_H);
    lv_obj_clear_flag(batt_body, LV_OBJ_FLAG_SCROLLABLE);

    batt_nub = lv_obj_create(screen);
    lv_obj_remove_style_all(batt_nub);
    lv_obj_add_style(batt_nub, &style_batt_lit, 0);
    lv_obj_set_size(batt_nub, BATT_NUB_W, BATT_NUB_H);
    lv_obj_clear_flag(batt_nub, LV_OBJ_FLAG_SCROLLABLE);

    /* fill: child of the body; position + size set per mode in battery_update
     * (inset level bar when discharging, full interior when charging). */
    batt_fill = lv_obj_create(batt_body);
    lv_obj_remove_style_all(batt_fill);
    lv_obj_add_style(batt_fill, &style_batt_lit, 0);
    lv_obj_set_size(batt_fill, 0, BATT_FILL_H);
    lv_obj_align(batt_fill, LV_ALIGN_TOP_LEFT, 1, 1);
    lv_obj_clear_flag(batt_fill, LV_OBJ_FLAG_SCROLLABLE);

    /* bolt: 1bpp canvas, drawn last so it sits on top of the fill. Its
     * background is LIT (palette 1 = black) so it merges with the solid
     * charging fill, and the bolt pixels are OFF (palette 0 = white) so the
     * lightning reads as a dark cut-out. Spans the full interior height,
     * centred horizontally. Hidden unless charging. */
    batt_bolt = lv_canvas_create(batt_body);
    lv_obj_remove_style_all(batt_bolt);
    lv_canvas_set_buffer(batt_bolt, bolt_cbuf, BOLT_W, BOLT_H, LV_COLOR_FORMAT_I1);
    lv_canvas_set_palette(batt_bolt, 0, lv_color_to_32(lv_color_white(), LV_OPA_COVER));
    lv_canvas_set_palette(batt_bolt, 1, lv_color_to_32(lv_color_black(), LV_OPA_COVER));
    lv_canvas_fill_bg(batt_bolt, lv_color_black(), LV_OPA_COVER); /* lit background */
    for (int32_t by = 0; by < BOLT_H; by++) {
        for (int32_t bx = 0; bx < BOLT_W; bx++) {
            if ((bolt_rows[by] >> (BOLT_W - 1 - bx)) & 1) {
                lv_canvas_set_px(batt_bolt, bx, by, lv_color_white(), LV_OPA_COVER); /* OFF */
            }
        }
    }
    lv_obj_align(batt_bolt, LV_ALIGN_TOP_LEFT, (BATT_INNER_W - BOLT_W) / 2, 0);
    lv_obj_add_flag(batt_bolt, LV_OBJ_FLAG_HIDDEN);

    /* percentage label: UNSCII 8 (true 1bpp, no anti-aliasing). */
    battery_label = lv_label_create(screen);
    lv_obj_set_style_text_font(battery_label, &lv_font_unscii_8, 0);
    lv_label_set_text(battery_label, "");

    lv_style_init(&style_cell_default);
    lv_style_set_radius(&style_cell_default, CELL_RADIUS);
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
        /* UNSCII 16: true 1bpp font (no anti-aliasing). The default 4bpp
         * Montserrat thresholds asymmetrically when the digit inverts in the
         * CHECKED state, so the active digit's stems render ~1 px thinner than
         * the inactive ones. A 1bpp glyph is pixel-identical in both states. */
        lv_obj_set_style_text_font(digit, &lv_font_unscii_16, 0);
        lv_label_set_text(digit, labels[i]);
        lv_obj_center(digit);
        profile_cells[i] = cell;
    }

    widget_battery_init();
    widget_profile_init();

    return screen;
}
