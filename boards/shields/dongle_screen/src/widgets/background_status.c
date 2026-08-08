/*
 * SPDX-License-Identifier: MIT
 *
 * See background_status.h.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include "background_status.h"

// Dim gold rather than a bright yellow, and never drawn at full opacity. The
// eyes and the dialogue are pure white at full strength, so the gap between
// them and this has to be wide enough that the background never reads as
// something to look at.
#define BG_COLOR 0xC79A00
#define BG_PEAK_OPA 140

// One shape at a range of sizes, rather than several shapes. A four-pointed
// star is legible down to a few pixels, where anything more detailed turns to
// mush, and keeping to one keeps the point maths in a single place.
#define BG_R_MIN 3
#define BG_R_MAX 9
// Waist radius as a share of the tip radius. Lower is spikier; much above this
// and the star rounds off into a diamond.
#define BG_WAIST_PCT 34
#define BG_LINE_W 2

// The whole burst, within which each sparkle takes its own turn. Long enough to
// register on boot, short enough not to delay the face.
#define BG_BURST_MS 2600
#define BG_IN_MS 400
#define BG_OUT_MS 700

// Keeps sparkles off the very edge, where the case lip eats the last few pixels
// of the panel.
#define BG_MARGIN 10

static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);

static uint32_t rnd_between(uint32_t lo, uint32_t hi) {
    return lo + (sys_rand32_get() % (hi - lo + 1));
}

// Tips on the axes, waists on the diagonals. 181/256 is sin(45), which puts a
// diagonal point that fraction of the waist radius along each axis. All
// coordinates are shifted by r so the shape sits inside a box of 2r, which is
// what lv_line expects - its points are relative to the object, not the centre.
static void build_sparkle(lv_point_precise_t *p, int32_t r) {
    const int32_t w = r * BG_WAIST_PCT / 100;
    const int32_t d = w * 181 / 256;

    const int32_t x[8] = {0, d, r, d, 0, -d, -r, -d};
    const int32_t y[8] = {-r, -d, 0, d, r, d, 0, -d};

    for (int i = 0; i < 8; i++) {
        p[i].x = r + x[i];
        p[i].y = r + y[i];
    }

    // Closed, so the outline has no gap where it started.
    p[8] = p[0];
}

static void sparkle_opa_cb(void *var, int32_t v) {
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)v, LV_PART_MAIN);
}

static void sparkle_done(lv_anim_t *a) {
    lv_obj_add_flag((lv_obj_t *)a->var, LV_OBJ_FLAG_HIDDEN);
}

void zmk_widget_background_sparkle_burst(struct zmk_widget_background *widget) {
    const int32_t w = lv_display_get_horizontal_resolution(NULL);
    const int32_t h = lv_display_get_vertical_resolution(NULL);

    for (int i = 0; i < BG_SPARKLES; i++) {
        lv_obj_t *o = widget->sparkle[i];
        const int32_t r = (int32_t)rnd_between(BG_R_MIN, BG_R_MAX);

        lv_anim_delete(o, sparkle_opa_cb);

        build_sparkle(widget->pts[i], r);
        lv_line_set_points(o, widget->pts[i], BG_SPARKLE_PTS);
        lv_obj_set_size(o, 2 * r + 1, 2 * r + 1);

        // Placed by its top-left, so the centre is offset back by the radius.
        lv_obj_set_pos(o, (int32_t)rnd_between(BG_MARGIN, w - BG_MARGIN) - r,
                       (int32_t)rnd_between(BG_MARGIN, h - BG_MARGIN) - r);

        lv_obj_set_style_opa(o, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);

        // Up and back down in one animation, via the playback. Each waits its
        // own turn, so they arrive scattered in time as well as in space, and
        // the delay is bounded so every sparkle is finished inside the burst.
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, o);
        lv_anim_set_exec_cb(&a, sparkle_opa_cb);
        lv_anim_set_values(&a, LV_OPA_TRANSP, BG_PEAK_OPA);
        lv_anim_set_delay(&a, rnd_between(0, BG_BURST_MS - BG_IN_MS - BG_OUT_MS));
        lv_anim_set_time(&a, BG_IN_MS);
        lv_anim_set_playback_time(&a, BG_OUT_MS);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
        lv_anim_set_completed_cb(&a, sparkle_done);
        lv_anim_start(&a);
    }
}

int zmk_widget_background_init(struct zmk_widget_background *widget, lv_obj_t *parent) {
    widget->obj = lv_obj_create(parent);
    lv_obj_remove_style_all(widget->obj);
    lv_obj_set_size(widget->obj, lv_pct(100), lv_pct(100));
    lv_obj_remove_flag(widget->obj, LV_OBJ_FLAG_SCROLLABLE);

    // Nothing here should ever intercept a redraw ordering decision made by the
    // widgets in front: this layer is created first and never raises itself.
    for (int i = 0; i < BG_SPARKLES; i++) {
        lv_obj_t *o = lv_line_create(widget->obj);
        widget->sparkle[i] = o;

        lv_obj_remove_style_all(o);
        lv_obj_set_style_line_color(o, lv_color_hex(BG_COLOR), LV_PART_MAIN);
        lv_obj_set_style_line_width(o, BG_LINE_W, LV_PART_MAIN);
        lv_obj_set_style_line_rounded(o, true, LV_PART_MAIN);
        lv_obj_set_style_pad_all(o, 0, LV_PART_MAIN);
        lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }

    sys_slist_append(&widgets, &widget->node);

    return 0;
}

lv_obj_t *zmk_widget_background_obj(struct zmk_widget_background *widget) { return widget->obj; }
