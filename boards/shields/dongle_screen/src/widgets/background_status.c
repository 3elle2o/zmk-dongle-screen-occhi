/*
 * SPDX-License-Identifier: MIT
 *
 * See background_status.h.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/display.h>
#include <zmk/event_manager.h>
#include <zmk/events/wpm_state_changed.h>
#include <zmk/wpm.h>

#include "background_status.h"

// Dim gold rather than a bright yellow, and never drawn at full opacity. The
// eyes and the dialogue are pure white at full strength, so the gap between
// them and this has to be wide enough that the background never reads as
// something to look at.
#define BG_COLOR 0xC79A00
#define BG_PEAK_OPA 210

// One shape at a range of sizes, rather than several shapes. A four-pointed
// star is legible down to a few pixels, where anything more detailed turns to
// mush, and keeping to one keeps the point maths in a single place.
#define BG_R_MIN 3
#define BG_R_MAX 12
// Waist radius as a share of the tip radius. Lower is spikier; much above this
// and the star rounds off into a diamond.
#define BG_WAIST_PCT 34

// Stroke scales with the star, so a big one does not come out as a thin wiry
// outline while a small one is a blob. Held to whole pixels, which is all there
// is to work with at these sizes.
#define BG_LINE_W(r) (1 + (r) / 5)

// The whole burst, within which each sparkle takes its own turn. Long enough to
// register on boot, short enough not to delay the face.
#define BG_BURST_MS 2600
#define BG_IN_MS 400
#define BG_OUT_MS 700

// Keeps sparkles off the very edge, where the case lip eats the last few pixels
// of the panel.
#define BG_MARGIN 10

// --- Stress lines ---
//
// Purple, and darker than the gold: these are pressure rather than sparkle, and
// there are sixteen of them across the top where a sparkle is one small shape.
#define BG_STRESS_COLOR 0x7B3FB0
#define BG_STRESS_MAX_OPA 200
#define BG_STRESS_W 3
#define BG_STRESS_LEN_MIN 16
#define BG_STRESS_LEN_MAX 74

// A blue wash behind the strokes. Vertical gradient into black, which is the
// screen's own colour, so it fades out rather than ending on an edge. Taller
// than the longest stroke so they finish inside it rather than hanging past it.
#define BG_GRAD_COLOR 0x263A96
#define BG_GRAD_H 96
#define BG_GRAD_MAX_OPA 130
// The wash reaches down further as the pressure builds, so it grows rather than
// only brightening. Starts shallow rather than at nothing, since a gradient a
// couple of pixels tall is a line, not a wash.
#define BG_GRAD_H_MIN 22

// Everything on this layer eases toward a new level rather than stepping to it.
// ZMK reports typing speed about once a second, so without this the effect
// arrives in visible jumps - and the layer is meant to swell, not tick.
#define BG_RAMP_MS 700

// Where the effect starts and where it reaches full strength. Matched to the
// eyes' own thresholds so the face and the background agree about what fast
// means: the strokes begin as the eyes start to strain and are at full pressure
// by the time the eyes give up entirely.
#define BG_STRESS_WPM_ON 57
#define BG_STRESS_WPM_FULL 87

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
        lv_obj_set_style_line_width(o, BG_LINE_W(r), LV_PART_MAIN);

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

// Scatters the strokes afresh: new positions, lengths and weights. Called each
// time the effect comes up from nothing, so a burst of fast typing does not
// draw the identical pattern it drew last time.
static void scatter_stress(struct zmk_widget_background *widget) {
    const int32_t w = lv_display_get_horizontal_resolution(NULL);

    for (int i = 0; i < BG_STRESS_LINES; i++) {
        const int32_t len = (int32_t)rnd_between(BG_STRESS_LEN_MIN, BG_STRESS_LEN_MAX);
        lv_obj_t *o = widget->stress[i];

        // Points are inset by half the stroke so a 2px line is not drawn half
        // outside the object it belongs to.
        widget->stress_pts[i][0].x = BG_STRESS_W / 2;
        widget->stress_pts[i][0].y = 0;
        widget->stress_pts[i][1].x = BG_STRESS_W / 2;
        widget->stress_pts[i][1].y = len;

        lv_line_set_points(o, widget->stress_pts[i], 2);
        lv_obj_set_size(o, BG_STRESS_W + 1, len + 1);

        // Spread across the width and hung from the very top edge, which is
        // where this effect reads from.
        lv_obj_set_pos(o, (int32_t)rnd_between(0, w - BG_STRESS_W), 0);

        widget->stress_weight[i] = (uint8_t)rnd_between(55, 100);
    }
}

// 0 at the threshold, 100 at full pressure. Below the threshold there is no
// effect at all rather than a very faint one, so ordinary typing leaves the
// background alone.
static uint8_t stress_intensity(uint8_t wpm) {
    if (wpm <= BG_STRESS_WPM_ON) {
        return 0;
    }
    if (wpm >= BG_STRESS_WPM_FULL) {
        return 100;
    }
    return (uint8_t)(((uint32_t)(wpm - BG_STRESS_WPM_ON) * 100) /
                     (BG_STRESS_WPM_FULL - BG_STRESS_WPM_ON));
}

struct bg_state {
    uint8_t wpm;
};

// What is on screen right now, as opposed to what the typing speed is asking
// for. Held separately so a change part-way through a ramp carries on from
// where it had reached rather than snapping back to start again.
static uint8_t bg_level;

// Draws the layer at a given level, 0 to 100. The animation exec callback, so
// every intermediate value passes through here and the wash and the strokes
// stay in step by construction.
static void bg_apply(void *var, int32_t v) {
    struct zmk_widget_background *widget = var;
    bg_level = (uint8_t)v;

    if (v <= 0) {
        lv_obj_add_flag(widget->stress_grad, LV_OBJ_FLAG_HIDDEN);
        for (int i = 0; i < BG_STRESS_LINES; i++) {
            lv_obj_add_flag(widget->stress[i], LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    lv_obj_set_height(widget->stress_grad,
                      BG_GRAD_H_MIN + ((BG_GRAD_H - BG_GRAD_H_MIN) * v) / 100);
    lv_obj_set_style_opa(widget->stress_grad, (lv_opa_t)((BG_GRAD_MAX_OPA * v) / 100),
                         LV_PART_MAIN);
    lv_obj_remove_flag(widget->stress_grad, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < BG_STRESS_LINES; i++) {
        const uint32_t opa = ((uint32_t)BG_STRESS_MAX_OPA * v * widget->stress_weight[i]) / 10000;
        lv_obj_set_style_opa(widget->stress[i], (lv_opa_t)opa, LV_PART_MAIN);
        lv_obj_remove_flag(widget->stress[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void bg_update_cb(struct bg_state state) {
    static uint8_t target;
    const uint8_t now = stress_intensity(state.wpm);

    if (now == target) {
        return;
    }

    struct zmk_widget_background *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
        // Only on the way up from nothing, so the pattern holds still for the
        // duration of an episode instead of reshuffling as the level moves.
        if (target == 0 && now > 0) {
            scatter_stress(widget);
        }

        lv_anim_delete(widget, bg_apply);

        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, widget);
        lv_anim_set_exec_cb(&a, bg_apply);
        lv_anim_set_values(&a, bg_level, now);
        lv_anim_set_time(&a, BG_RAMP_MS);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_start(&a);
    }

    target = now;
}

static struct bg_state bg_get_state(const zmk_event_t *eh) {
    ARG_UNUSED(eh);
    // Read from the API rather than the event payload, so the one-off init call
    // that arrives with a null event needs no special case.
    return (struct bg_state){.wpm = (uint8_t)zmk_wpm_get_state()};
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_background, struct bg_state, bg_update_cb, bg_get_state)
ZMK_SUBSCRIPTION(widget_background, zmk_wpm_state_changed);

int zmk_widget_background_init(struct zmk_widget_background *widget, lv_obj_t *parent) {
    widget->obj = lv_obj_create(parent);
    lv_obj_remove_style_all(widget->obj);
    lv_obj_set_size(widget->obj, lv_pct(100), lv_pct(100));
    lv_obj_remove_flag(widget->obj, LV_OBJ_FLAG_SCROLLABLE);

    // Creation order is the whole layering scheme, here as much as between
    // widgets: the wash first so it is behind its own strokes, and behind the
    // sparkles too if a power-up burst ever coincides with hard typing.
    widget->stress_grad = lv_obj_create(widget->obj);
    lv_obj_remove_style_all(widget->stress_grad);
    lv_obj_set_size(widget->stress_grad, lv_pct(100), BG_GRAD_H);
    lv_obj_set_pos(widget->stress_grad, 0, 0);
    lv_obj_set_style_bg_color(widget->stress_grad, lv_color_hex(BG_GRAD_COLOR), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(widget->stress_grad, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(widget->stress_grad, LV_GRAD_DIR_VER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(widget->stress_grad, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_add_flag(widget->stress_grad, LV_OBJ_FLAG_HIDDEN);

    // Nothing here should ever intercept a redraw ordering decision made by the
    // widgets in front: this layer is created first and never raises itself.
    for (int i = 0; i < BG_SPARKLES; i++) {
        lv_obj_t *o = lv_line_create(widget->obj);
        widget->sparkle[i] = o;

        lv_obj_remove_style_all(o);
        lv_obj_set_style_line_color(o, lv_color_hex(BG_COLOR), LV_PART_MAIN);
        // Width is set per sparkle in the burst, since it follows the size.
        lv_obj_set_style_line_rounded(o, true, LV_PART_MAIN);
        lv_obj_set_style_pad_all(o, 0, LV_PART_MAIN);
        lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }

    // Created after the sparkles, so they sit over them if the two ever
    // coincide - a burst on power-up and hard typing at the same moment.
    for (int i = 0; i < BG_STRESS_LINES; i++) {
        lv_obj_t *o = lv_line_create(widget->obj);
        widget->stress[i] = o;

        lv_obj_remove_style_all(o);
        lv_obj_set_style_line_color(o, lv_color_hex(BG_STRESS_COLOR), LV_PART_MAIN);
        lv_obj_set_style_line_width(o, BG_STRESS_W, LV_PART_MAIN);
        lv_obj_set_style_pad_all(o, 0, LV_PART_MAIN);
        lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }

    scatter_stress(widget);

    sys_slist_append(&widgets, &widget->node);

    widget_background_init();

    return 0;
}

lv_obj_t *zmk_widget_background_obj(struct zmk_widget_background *widget) { return widget->obj; }
