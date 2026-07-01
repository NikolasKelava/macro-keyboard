/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 * Custom status screen for macro_keyboard's 128x64 SSD1306, rendered
 * in 90° (portrait) orientation: 64 px wide x 128 px tall.
 *
 *   +-------------+   <- top of the device once rotated to the right
 *   |  [==] NN%   |   battery row, centred (custom icon + UNSCII 8)
 *   | CT:     BLE |   connection: "CT:" pinned left, USB/BLE pinned right
 *   | EM:    VSCR |   encoder mode: "EM:" pinned left, value pinned right
 *   |  ( MEDIA )  |   ACTIVE profile = tall lit cell + dark name
 *   |  (#######)  |   inactive profiles = short lit pills (no letters), each
 *   |  (#######)  |   capped at 10 px; they only shrink below that when too
 *   |  (#######)  |   many profiles exist. Free space may remain at the bottom.
 *   +-------------+
 *
 * "active" is signalled by SIZE (a tall cell carrying the layer's name) rather
 * than by fill. On a layer change the previously-active cell is squeezed and
 * the new one expands; the column re-packs instantly (no animation).
 *
 * Hand-drawn cells: lit (bright) rounded bars on the dark background, with the
 * active cell's name cut into it (dark). LVGL's rounded-rectangle at
 * LV_COLOR_DEPTH_1 has no anti-aliasing (its corners collapse to a bevel), so
 * we rasterise crisp rounded shapes ourselves into ONE 1bpp canvas (same crisp
 * approach as the battery icon) and overlay the name as a UNSCII-8 label. The
 * canvas buffer is written DIRECTLY (one memset + bit-ORs, invalidated once),
 * NOT via lv_canvas_set_px / lv_canvas_fill_bg, which invalidate per pixel.
 *
 * Flexible count: the number of profiles is read live from the keymap
 * (1..MAX_PROFILES) rather than hardcoded, so it tracks however many layers
 * exist. Inactive cells are a uniform height capped at 10 px (only shrinking
 * below that, down to a 5 px floor, when too many profiles exist to fit); the
 * column is NOT stretched to the bottom edge, so free space may remain below.
 * Every cell uses the same fixed CELL_RADIUS round-off regardless of count.
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
 * white<->black only in our custom widget styles.
 *
 * Crisp battery row: LVGL renders 4bpp anti-aliased glyphs (Montserrat,
 * and the FontAwesome battery/charge LV_SYMBOL icons baked into it) by
 * thresholding each pixel's luminance at 127 onto the I1 panel — the soft
 * edges land near that threshold and produce bulky, fuzzy glyphs with
 * stray lit pixels. So all text on this screen uses lv_font_unscii_8
 * (a true 1bpp bitmap font, no anti-aliasing) and the battery icon is
 * drawn from full-opacity axis-aligned rectangles:
 *   - percentage uses lv_font_unscii_8 so the digits are pixel-exact;
 *   - the battery is body outline + terminal nub + level-proportional fill;
 *   - while charging the fill is replaced by a small lightning bolt drawn
 *     into a 1bpp lv_canvas (hand-set pixels, also crisp).
 * The whole battery row is re-centred on the canvas on every update.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <stdio.h>
#include <string.h>

#include <lvgl.h>

#include <zmk/battery.h>
#include <zmk/display.h>
#include <zmk/endpoints.h>
#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/keymap.h>
#include <zmk/usb.h>

#include "encoder_mode.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* Profile column — flexible count. The screen reads the live number of keymap
 * layers (1..MAX_PROFILES) and lays out that many cells top-down from
 * CELL_TOP_START. The ACTIVE cell is a fixed height (ACTIVE_H) carrying the
 * layer name; the inactive cells are a uniform height capped at MAX_SQUEEZED_H
 * (10 px) and only shrink below it — down to MIN_SQUEEZED_H, which bounds
 * MAX_PROFILES — when too many profiles exist to fit. The column is NOT
 * stretched to COLUMN_BOTTOM, so free space may remain below. Every cell uses
 * the SAME fixed CELL_RADIUS, so the round-off is identical at every count; the
 * cells are rasterised by hand (see profiles). */
#define CELL_W              64
#define ACTIVE_H            24    /* tall (active) cell height */
#define CELL_GAP             4    /* vertical gap between cells */
#define CELL_TOP_START      44    /* y of the first cell's top edge (64x128 logical) */
#define COLUMN_BOTTOM      124    /* cells stack down to at most this bottom edge */
#define MIN_SQUEEZED_H       5    /* floor for an inactive cell; bounds MAX_PROFILES */
#define MAX_SQUEEZED_H      10    /* cap for an inactive cell (don't grow past this) */
#define CELL_RADIUS          5    /* fixed round-off used by EVERY cell (active+inactive) */
#define PROFILE_LABEL_MAXLEN 7    /* UNSCII 8 is 8 px/glyph; 7 fit in CELL_W=64 */
#define TEXT_H               8    /* UNSCII 8 glyph/line height */

#define COLUMN_AVAIL   (COLUMN_BOTTOM - CELL_TOP_START)
/* Largest N for which the inactive cells stay >= MIN_SQUEEZED_H. */
#define MAX_PROFILES   (1 + (COLUMN_AVAIL - ACTIVE_H) / (MIN_SQUEEZED_H + CELL_GAP))

/* Status text rows between the battery and the profile area. Each row is split:
 * a fixed prefix ("CT:"/"EM:") pinned to the LEFT edge and the value
 * ("USB"/"BLE", "VOL"/"VSCR"/...) pinned to the RIGHT edge — so a value of a
 * different length only moves its own left edge, not the whole line. */
#define CONN_Y   17    /* connection row top */
#define ENC_Y    28    /* encoder-mode row top */
#define EDGE_PAD  1    /* inset of the left/right-edge text */

/* Profile canvas. The cells are hand-drawn into one 1bpp canvas spanning
 * BLOCK_TOP down to the screen bottom: lit (bright) rounded bars on the dark
 * background, the active cell's name cut into it (dark). No divider line. */
#define SCREEN_H  128
#define BLOCK_TOP  40    /* top edge of the profile canvas (a touch above cell 1) */

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

/* Charge-bolt art (7x9), centred into the charging canvas below. */
#define BOLT_W 7
#define BOLT_H 9

/* The charging graphic is ONE standalone I1 canvas the size of the battery
 * interior: a solid lit fill with the bolt punched out as a dark cut-out. It is
 * a DIRECT CHILD OF THE SCREEN (like col_canvas), NOT a clipped child of the
 * bordered battery body, and it does NOT overlap the level fill. A clipped image
 * child made LVGL spin up an RGB565 intermediate layer whose buffer math is
 * wrong in this 1bpp (LV_COLOR_DEPTH_1) build; its blend ran out of bounds and
 * took an MPU data-access fault (K_ERR_ARM_MEM_DATA_ACCESS) the instant the
 * charging icon drew — USB only, since the bolt only shows while charging. As a
 * standalone, unclipped, opaque canvas it renders on the safe I1 path. */
#define CHG_W BATT_INNER_W
#define CHG_H BATT_INNER_H

static lv_obj_t *battery_label;
static lv_obj_t *batt_body;          /* outlined battery body */
static lv_obj_t *batt_nub;           /* terminal nub on the right */
static lv_obj_t *batt_fill;          /* level-proportional fill (hidden while charging) */
static lv_obj_t *batt_bolt;          /* charge bolt canvas (hidden unless charging) */
static lv_obj_t *conn_prefix;        /* "CT:" pinned to the left edge */
static lv_obj_t *conn_value;         /* "USB" / "BLE" pinned to the right edge */
static lv_obj_t *enc_prefix;         /* "EM:" pinned to the left edge */
static lv_obj_t *enc_value;          /* "VOL"/"VSCR"/... pinned to the right edge */
static lv_obj_t *col_canvas;         /* hand-drawn profile cells (one 1bpp canvas) */
static lv_obj_t *name_label;         /* active profile name overlay */
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

/* I1 canvas buffers must reserve the 8-byte in-buffer palette (2 entries x 4 B)
 * that LV_CANVAS_BUF_SIZE omits, or lv_canvas_set_px / the render run past the
 * buffer end. Same reservation as col_cbuf below. */
#define I1_PALETTE_BYTES (LV_COLOR_INDEXED_PALETTE_SIZE(LV_COLOR_FORMAT_I1) * 4)
static uint8_t chg_cbuf[LV_CANVAS_BUF_SIZE(CHG_W, CHG_H, 1, LV_DRAW_BUF_STRIDE_ALIGN) +
                        I1_PALETTE_BYTES];

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
        /* Charging: show the standalone charge canvas (solid fill + bolt cut-out)
         * over the battery interior, and HIDE the level fill so nothing overlaps
         * it (the canvas IS the fill). Absolute screen coords: the body is at
         * (x0, BATT_ROW_Y) and its interior starts one border pixel in. */
        lv_obj_add_flag(batt_fill, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(batt_bolt, LV_ALIGN_TOP_LEFT, x0 + 1, BATT_ROW_Y + 1);
        lv_obj_clear_flag(batt_bolt, LV_OBJ_FLAG_HIDDEN);
    } else {
        /* Discharging: level bar inset 1 px inside the interior; canvas hidden. */
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

/* --------------------------------------------------------------- connection */

struct conn_state {
    enum zmk_transport transport;
};

static void conn_update(struct conn_state state) {
    lv_label_set_text(conn_value, state.transport == ZMK_TRANSPORT_USB ? "USB" : "BLE");
    lv_obj_align(conn_value, LV_ALIGN_TOP_RIGHT, -EDGE_PAD, CONN_Y);
}

static struct conn_state conn_get_state(const zmk_event_t *eh) {
    const struct zmk_endpoint_changed *ev = as_zmk_endpoint_changed(eh);
    return (struct conn_state){
        .transport = (ev != NULL) ? ev->endpoint.transport : zmk_endpoints_selected().transport,
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_conn, struct conn_state, conn_update, conn_get_state)
ZMK_SUBSCRIPTION(widget_conn, zmk_endpoint_changed);

/* ------------------------------------------------------------- encoder mode */

/* Short codes so "EM:<code>" stays within the 64 px row at UNSCII 8. */
static const char *enc_mode_text(enum encoder_mode mode) {
    switch (mode) {
    case ENC_MODE_VOLUME:
        return "VOL";
    case ENC_MODE_VSCROLL:
        return "VSCR";
    case ENC_MODE_HSCROLL:
        return "HSCR";
    case ENC_MODE_TABS:
        return "TABS";
    default:
        return "?";
    }
}

struct enc_mode_widget_state {
    enum encoder_mode mode;
};

static void enc_mode_update(struct enc_mode_widget_state state) {
    lv_label_set_text(enc_value, enc_mode_text(state.mode));
    lv_obj_align(enc_value, LV_ALIGN_TOP_RIGHT, -EDGE_PAD, ENC_Y);
}

static struct enc_mode_widget_state enc_mode_get_state(const zmk_event_t *eh) {
    /* Read the live per-profile mode regardless of which event woke us — both
     * &enc_mode_next (zmk_encoder_mode_changed) and a profile switch
     * (zmk_layer_state_changed) can change which mode is current. */
    return (struct enc_mode_widget_state){.mode = encoder_mode_get()};
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_enc_mode, struct enc_mode_widget_state,
                            enc_mode_update, enc_mode_get_state)
ZMK_SUBSCRIPTION(widget_enc_mode, zmk_encoder_mode_changed);
ZMK_SUBSCRIPTION(widget_enc_mode, zmk_layer_state_changed);

/* ----------------------------------------------------------------- profiles */

/* Hand-drawn, inverted profile block: LVGL's 1bpp rounded-rect bevels its
 * corners (no AA at this colour depth), so we rasterise crisp shapes ourselves.
 * The block is LIT everywhere except the cells, which are dark (OFF) rounded
 * cut-outs; the active name is overlaid LIT. ONE I1 canvas covers the whole
 * block (BLOCK_TOP .. screen bottom). */

#define COL_CANVAS_W     CELL_W
#define COL_CANVAS_H     (SCREEN_H - BLOCK_TOP)         /* full inverted-block height */
#define CELL_LOCAL_START (CELL_TOP_START - BLOCK_TOP)   /* first cell's y within the canvas */

/* Same in-buffer I1 palette reservation as chg_cbuf above (I1_PALETTE_BYTES):
 * LV_CANVAS_BUF_SIZE omits the 8-byte palette that lv_draw_buf_goto_xy skips
 * over, so without it the bitmap's last rows run past the end of the buffer. */
static uint8_t col_cbuf[LV_CANVAS_BUF_SIZE(COL_CANVAS_W, COL_CANVAS_H, 1, LV_DRAW_BUF_STRIDE_ALIGN) +
                        I1_PALETTE_BYTES];
static lv_draw_buf_t *col_dbuf; /* canvas draw buffer (cached) */
static uint8_t *col_bm;         /* bitmap start, past the palette */
static uint32_t col_stride;     /* bytes per row */

/* Light one cell pixel (bit 1 = lit cell over the bit-0 dark background). We
 * write the buffer DIRECTLY (col_bm starts past the palette, from
 * lv_draw_buf_goto_xy) rather than lv_canvas_set_px, because set_px invalidates
 * the object on EVERY call (and lv_canvas_fill_bg on an I1 canvas does the same
 * per pixel) — that was ~10k invalidations per profile switch, which made
 * switching laggy and stretched boot. profile_update invalidates once. */
static inline void col_cell_px(int32_t x, int32_t y) {
    if ((uint32_t)x >= COL_CANVAS_W || (uint32_t)y >= COL_CANVAS_H) {
        return;
    }
    col_bm[(uint32_t)y * col_stride + ((uint32_t)x >> 3)] |= (uint8_t)(1u << (7 - (x & 7)));
}

/* Draw a filled rounded rectangle (a lit cell) of radius r at canvas-local
 * (0, y0), size w x h, over the dark background. Distances use pixel CENTRES
 * in doubled integer units (2*coord+1) so the round-off is a true circle —
 * symmetric in x and y (the plain-integer version rounded asymmetrically on the
 * short bars). r == h/2 yields full semicircular caps (a pill). */
static void col_fill_rrect(int32_t y0, int32_t w, int32_t h, int32_t r) {
    if (r > h / 2) {
        r = h / 2;
    }
    if (r > w / 2) {
        r = w / 2;
    }
    const int32_t left = 2 * r, right = 2 * w - 2 * r;  /* inner-box edges, doubled (x) */
    const int32_t top = 2 * r, bottom = 2 * h - 2 * r;  /* inner-box edges, doubled (y) */
    const int32_t rr = (2 * r) * (2 * r);
    for (int32_t ly = 0; ly < h; ly++) {
        const int32_t py = 2 * ly + 1;
        const int32_t ny = (py < top) ? top : (py > bottom ? bottom : py);
        const int32_t dy = py - ny;
        for (int32_t lx = 0; lx < w; lx++) {
            const int32_t px = 2 * lx + 1;
            const int32_t nx = (px < left) ? left : (px > right ? right : px);
            const int32_t dx = px - nx;
            if (dx * dx + dy * dy <= rr) {
                col_cell_px(lx, y0 + ly);
            }
        }
    }
}

struct profile_state {
    uint8_t active;  /* index of the active profile (0-based)         */
    uint8_t count;   /* number of profiles currently present (1..MAX) */
};

/* Number of keymap layers currently present (contiguous valid indices from 0),
 * clamped to [1, MAX_PROFILES]. Without layer-reordering this is the static
 * keymap layer count; with it, it tracks Studio add/remove. */
static uint8_t profile_count(void) {
    uint8_t n = 0;
    while (n < MAX_PROFILES &&
           zmk_keymap_layer_index_to_id((zmk_keymap_layer_index_t)n) != ZMK_KEYMAP_LAYER_ID_INVAL) {
        n++;
    }
    return (n == 0) ? 1 : n;
}

/* First PROFILE_LABEL_MAXLEN chars of the layer's display-name (falls back to
 * the 1-based profile number if the layer is unnamed). */
static void profile_short_name(uint8_t idx, char *out, size_t out_sz) {
    zmk_keymap_layer_id_t id = zmk_keymap_layer_index_to_id((zmk_keymap_layer_index_t)idx);
    const char *name = (id != ZMK_KEYMAP_LAYER_ID_INVAL) ? zmk_keymap_layer_name(id) : NULL;
    if (name == NULL || name[0] == '\0') {
        snprintf(out, out_sz, "%u", (unsigned)(idx + 1));
        return;
    }
    size_t n = 0;
    while (name[n] != '\0' && n < out_sz - 1) {
        n++;
    }
    memcpy(out, name, n);
    out[n] = '\0';
}

static void profile_update(struct profile_state state) {
    const uint8_t count = (state.count < 1) ? 1 : state.count;
    const uint8_t active = (state.active < count) ? state.active : 0;

    /* Inactive cell height: capped at MAX_SQUEEZED_H. We do NOT stretch the
     * column to the bottom edge — at low counts the cells stay 10 px and there
     * is simply free space below. They only shrink below the cap when there are
     * too many profiles to fit them all (floored at MIN_SQUEEZED_H, which is
     * what bounds MAX_PROFILES). Height is uniform across inactive cells. The
     * radius is the SAME fixed CELL_RADIUS for every cell regardless of count,
     * so the round-off never changes (col_fill_rrect clamps it to h/2 only for
     * the rare sub-10 px cells at the highest counts). */
    int32_t squeezed = MAX_SQUEEZED_H;
    if (count >= 2) {
        const int32_t inactive_total = COLUMN_AVAIL - ACTIVE_H - (count - 1) * CELL_GAP;
        const int32_t per_cell = inactive_total / (count - 1);
        if (per_cell < squeezed) {
            squeezed = per_cell;
        }
        if (squeezed < MIN_SQUEEZED_H) { /* capacity guard (count > MAX_PROFILES) */
            squeezed = MIN_SQUEEZED_H;
        }
    }

    /* Clear to the dark background (bit 0), then draw the lit cells (bit 1)
     * directly. memset on LVGL's reported data is far cheaper than
     * lv_canvas_fill_bg (which set_px's every pixel and invalidates each). */
    memset(col_bm, 0x00, (size_t)col_stride * COL_CANVAS_H);

    int32_t y = CELL_LOCAL_START; /* canvas-local top of the next cell */
    int32_t active_local_y = CELL_LOCAL_START;
    for (uint8_t i = 0; i < count; i++) {
        int32_t h;
        if (i == active) {
            h = ACTIVE_H;
            active_local_y = y;
        } else {
            h = squeezed;
        }
        col_fill_rrect(y, CELL_W, h, CELL_RADIUS);
        y += h + CELL_GAP;
    }
    lv_obj_invalidate(col_canvas);

    /* Overlay the active profile's name (LIT) centred in its dark cell. The
     * canvas sits at BLOCK_TOP, so active_local_y is relative to that. */
    char buf[PROFILE_LABEL_MAXLEN + 1];
    profile_short_name(active, buf, sizeof(buf));
    lv_label_set_text(name_label, buf);
    lv_obj_align(name_label, LV_ALIGN_TOP_MID, 0,
                 BLOCK_TOP + active_local_y + (ACTIVE_H - TEXT_H) / 2);
}

static struct profile_state profile_get_state(const zmk_event_t *eh) {
    const uint8_t count = profile_count();
    zmk_keymap_layer_index_t idx = zmk_keymap_highest_layer_active();
    return (struct profile_state){
        .active = (idx < count) ? (uint8_t)idx : 0,
        .count = count,
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
    lv_obj_set_style_pad_all(screen, 0, 0); /* so left/right-edge text hits the true edges */

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

    /* charging graphic: ONE standalone I1 canvas the size of the battery interior
     * — a solid lit background (palette 1 = black = lit) with the bolt punched
     * out as OFF (palette 0 = white) dark pixels, centred. A DIRECT CHILD OF
     * screen (like col_canvas), positioned over the battery interior in
     * battery_update; hidden unless charging. Being standalone + unclipped +
     * opaque keeps it on LVGL's safe I1 render path (see the CHG_W note — a
     * clipped image child faulted via an RGB565 intermediate layer). */
    batt_bolt = lv_canvas_create(screen);
    lv_obj_remove_style_all(batt_bolt);
    lv_canvas_set_buffer(batt_bolt, chg_cbuf, CHG_W, CHG_H, LV_COLOR_FORMAT_I1);
    lv_canvas_set_palette(batt_bolt, 0, lv_color_to_32(lv_color_white(), LV_OPA_COVER)); /* OFF */
    lv_canvas_set_palette(batt_bolt, 1, lv_color_to_32(lv_color_black(), LV_OPA_COVER)); /* LIT */
    lv_canvas_fill_bg(batt_bolt, lv_color_black(), LV_OPA_COVER); /* solid lit fill */
    for (int32_t by = 0; by < BOLT_H; by++) {
        for (int32_t bx = 0; bx < BOLT_W; bx++) {
            if ((bolt_rows[by] >> (BOLT_W - 1 - bx)) & 1) {
                lv_canvas_set_px(batt_bolt, (CHG_W - BOLT_W) / 2 + bx, by, lv_color_white(),
                                 LV_OPA_COVER); /* OFF cut-out */
            }
        }
    }
    lv_obj_add_flag(batt_bolt, LV_OBJ_FLAG_HIDDEN);

    /* percentage label: UNSCII 8 (true 1bpp, no anti-aliasing). */
    battery_label = lv_label_create(screen);
    lv_obj_set_style_text_font(battery_label, &lv_font_unscii_8, 0);
    lv_label_set_text(battery_label, "");

    /* ---- connection (CT:) + encoder-mode (EM:) rows: fixed prefix pinned to
     * the LEFT edge, value pinned to the RIGHT edge (set in the widgets). ---- */
    conn_prefix = lv_label_create(screen);
    lv_obj_set_style_text_font(conn_prefix, &lv_font_unscii_8, 0);
    lv_label_set_text(conn_prefix, "CT:");
    lv_obj_align(conn_prefix, LV_ALIGN_TOP_LEFT, EDGE_PAD, CONN_Y);

    conn_value = lv_label_create(screen);
    lv_obj_set_style_text_font(conn_value, &lv_font_unscii_8, 0);
    lv_label_set_text(conn_value, "");
    lv_obj_align(conn_value, LV_ALIGN_TOP_RIGHT, -EDGE_PAD, CONN_Y);

    enc_prefix = lv_label_create(screen);
    lv_obj_set_style_text_font(enc_prefix, &lv_font_unscii_8, 0);
    lv_label_set_text(enc_prefix, "EM:");
    lv_obj_align(enc_prefix, LV_ALIGN_TOP_LEFT, EDGE_PAD, ENC_Y);

    enc_value = lv_label_create(screen);
    lv_obj_set_style_text_font(enc_value, &lv_font_unscii_8, 0);
    lv_label_set_text(enc_value, "");
    lv_obj_align(enc_value, LV_ALIGN_TOP_RIGHT, -EDGE_PAD, ENC_Y);

    /* ---- inverted profile block: one hand-drawn 1bpp canvas whose background
     * is LIT and whose cells are dark (OFF) rounded cut-outs, plus a single
     * UNSCII-8 overlay carrying the active profile's name (LIT, so it reads as
     * bright letters on the dark active cell). profile_update fills the block,
     * punches the cells and positions the name. ---- */
    col_canvas = lv_canvas_create(screen);
    lv_obj_remove_style_all(col_canvas);
    lv_canvas_set_buffer(col_canvas, col_cbuf, COL_CANVAS_W, COL_CANVAS_H, LV_COLOR_FORMAT_I1);
    lv_canvas_set_palette(col_canvas, 0, lv_color_to_32(lv_color_white(), LV_OPA_COVER)); /* OFF */
    lv_canvas_set_palette(col_canvas, 1, lv_color_to_32(lv_color_black(), LV_OPA_COVER)); /* LIT */
    lv_obj_align(col_canvas, LV_ALIGN_TOP_MID, 0, BLOCK_TOP);
    col_dbuf = lv_canvas_get_draw_buf(col_canvas);
    col_bm = lv_draw_buf_goto_xy(col_dbuf, 0, 0); /* bitmap start (skips the I1 palette) */
    col_stride = col_dbuf->header.stride;

    name_label = lv_label_create(screen);
    lv_obj_set_style_text_font(name_label, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(name_label, lv_color_white(), 0); /* OFF -> dark on lit cell */
    lv_obj_set_style_bg_opa(name_label, LV_OPA_TRANSP, 0);
    lv_label_set_text(name_label, "");

    widget_battery_init();
    widget_conn_init();
    widget_enc_mode_init();
    widget_profile_init();

    return screen;
}
