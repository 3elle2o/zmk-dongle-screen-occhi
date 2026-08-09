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
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/wpm_state_changed.h>
#include <zmk/keymap.h>
#include <zmk/wpm.h>

#include "background_status.h"
#include <fonts.h>

// Dim gold rather than a bright yellow, and never drawn at full opacity. The
// eyes and the dialogue are pure white at full strength, so the gap between
// them and this has to be wide enough that the background never reads as
// something to look at.
#define BG_COLOR 0xC79A00
#define BG_PEAK_OPA 210

// One shape at a range of sizes, rather than several shapes. A four-pointed
// star is legible down to a few pixels, where anything more detailed turns to
// mush, and keeping to one keeps the point maths in a single place.
#define BG_R_MIN 6
#define BG_R_MAX 18
// Waist radius as a share of the tip radius. Lower is spikier; much above this
// and the star rounds off into a diamond.
#define BG_WAIST_PCT 34

// Stroke scales with the star, so a big one does not come out as a thin wiry
// outline while a small one is a blob. Held to whole pixels, which is all there
// is to work with at these sizes.
#define BG_LINE_W(r) (1 + (r) / 5)

// How long an effect stays up for once released. Also the sparkles' pulse
// period, so a burst at power-up breathes once and goes.
#define BG_BURST_MS 2600
#define BG_IN_MS 400

// Keeps sparkles off the very edge, where the case lip eats the last few pixels
// of the panel.
#define BG_MARGIN 10

// --- Anger marks ---
//
// Bright, and the one thing on this layer that is meant to be: the rest is
// atmosphere, this is the face shouting. Still short of the pure white in
// front of it.
#define BG_ANGER_COLOR 0xFF2A2A
#define BG_ANGER_MAX_OPA 210
#define BG_ANGER_R_MIN 7
#define BG_ANGER_R_MAX 16
// How near the centre each stroke sags at its middle, against the reach its
// ends keep. Lower is a deeper bow and a wider hollow.
#define BG_ANGER_INNER_PCT 52
// How much of its quarter each stroke actually covers. Below 100 a gap opens at
// every diagonal, and the gap is the point - at 100 the four strokes meet and
// it reads as one ring rather than four veins. Round caps eat into it as well,
// so the gap on screen is always smaller than the angle suggests.
#define BG_ANGER_SPAN_PCT 65

// Heavier than a sparkle's outline. A sparkle is a glint and can be wiry; this
// is meant to look like something under pressure.
#define BG_ANGER_LINE_W(r) (2 + (r) / 5)

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

#define TRIG_MAX 32767

static int32_t sin_of(int32_t deg) {
    while (deg < 0) {
        deg += 360;
    }
    return lv_trigo_sin((int16_t)(deg % 360));
}

static int32_t cos_of(int32_t deg) { return sin_of(deg + 90); }

// Tips on the axes, waists on the diagonals. 181/256 is sin(45), which puts a
// diagonal point that fraction of the waist radius along each axis. All
// coordinates are shifted by r so the shape sits inside a box of 2r, which is
// what lv_line expects - its points are relative to the object, not the centre.
static void build_sparkle(lv_point_precise_t *p, int32_t r, int32_t inset) {
    // Traced inside the box by half the stroke, because a rounded cap reaches
    // that far past the point it ends on and LVGL clips an object's drawing to
    // its own area. Without this the tips come back shaved.
    const int32_t reach = r - inset;
    const int32_t w = reach * BG_WAIST_PCT / 100;
    const int32_t d = w * 181 / 256;

    const int32_t x[8] = {0, d, reach, d, 0, -d, -reach, -d};
    const int32_t y[8] = {-reach, -d, 0, d, reach, d, 0, -d};

    for (int i = 0; i < 8; i++) {
        p[i].x = r + x[i];
        p[i].y = r + y[i];
    }

    // Closed, so the outline has no gap where it started.
    p[8] = p[0];
}

// One stroke of the mark, k choosing which quarter it faces.
//
// Radius is carried out at both ends and in at the middle, following a square
// so it leaves each end quickly and flattens through the bow. Working in polar
// keeps this to one expression; the same shape as a Bezier would need control
// points nobody could read.
static void build_anger_arc(lv_point_precise_t *p, int32_t r, int k, int32_t inset) {
    // As with the sparkle: the stroke's rounded cap reaches half its width past
    // the outermost point, so the reach comes in by that much or the tips are
    // clipped against the object's own edge.
    const int32_t reach = r - inset;
    const int32_t r_in = reach * BG_ANGER_INNER_PCT / 100;
    const int32_t half = (90 * BG_ANGER_SPAN_PCT / 100) / 2;
    const int32_t last = BG_ANGER_ARC_PTS - 1;

    for (int i = 0; i < BG_ANGER_ARC_PTS; i++) {
        const int32_t deg = 90 * k - half + (2 * half * i) / last;

        // (2i/last - 1) squared, kept in integers by scaling both terms.
        const int32_t t = 2 * i - last;
        const int32_t rad = r_in + ((reach - r_in) * t * t) / (last * last);

        p[i].x = r + (rad * cos_of(deg)) / TRIG_MAX;
        p[i].y = r + (rad * sin_of(deg)) / TRIG_MAX;
    }
}

static void sparkle_opa_cb(void *var, int32_t v) {
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)v, LV_PART_MAIN);
}

// --- Layer effects ---
//
// One machine for all three. Each is a scattering of small marks that pulse for
// as long as the layer is held, and each is taken down the same way: not when
// the key comes up, but once it has been on screen for the standard burst. A
// tap and a hold therefore differ only in how long the pulsing runs, and a tap
// is not a flicker.

enum bg_effect {
    BG_EFFECT_NONE = 0,
    BG_EFFECT_SPARKLE,
    BG_EFFECT_SYMBOLS,
    BG_EFFECT_ANGER,
};

// Which layer summons what. Deliberately its own table rather than anything
// derived from the eyes: that maps layers to expressions, this maps layers to
// atmosphere, and the symbol layer has an entry here while having no expression
// at all.
static const uint8_t layer_effect[] = {
    [1] = BG_EFFECT_SYMBOLS,
    [2] = BG_EFFECT_SPARKLE,
    [3] = BG_EFFECT_ANGER,
};

// Punctuation only - no letters, or it reads as words rather than as symbols.
static const char *const BG_SYMBOL_CHARS[] = {"&", "[", "]", "?", "#", "@", "*",
                                              "+", "/", "<", ">", "=", "{", "}",
                                              ";", ":", "~", "^", "%", "$"};

#define BG_SYMBOL_COLOR 0x2FA8BE
#define BG_SYMBOL_MAX_OPA 190

// How long a mark takes to breathe in and out once. The sparkles keep their
// slower original pace; the other two are brisker, being reactions to a key.
#define BG_PULSE_MS 1600

// Starts one mark pulsing: invisible, then breathing between nothing and its
// peak forever, offset so the group never beats in unison.
static void pulse(lv_obj_t *o, lv_opa_t peak, uint32_t cycle_ms, uint32_t delay_ms) {
    lv_anim_delete(o, sparkle_opa_cb);
    lv_obj_set_style_opa(o, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, o);
    lv_anim_set_exec_cb(&a, sparkle_opa_cb);
    lv_anim_set_values(&a, LV_OPA_TRANSP, peak);
    lv_anim_set_delay(&a, delay_ms);
    lv_anim_set_time(&a, cycle_ms / 2);
    lv_anim_set_playback_time(&a, cycle_ms / 2);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

static void quiet(lv_obj_t *o) {
    lv_anim_delete(o, sparkle_opa_cb);
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

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
    uint8_t layer;
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

// Defined below, alongside the rest of the effect machinery.
static void bg_set_effect(uint8_t effect);

static void bg_update_cb(struct bg_state state) {
    static uint8_t target;

    // Layer first, then the typing ramp. Both run here rather than in their own
    // listeners so that everything touching LVGL is on the display thread.
    bg_set_effect(state.layer < ARRAY_SIZE(layer_effect) ? layer_effect[state.layer]
                                                         : BG_EFFECT_NONE);
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
    // Read from the APIs rather than the event payload, so the one-off init
    // call that arrives with a null event needs no special case, and one
    // callback can serve both subscriptions.
    return (struct bg_state){
        .wpm = (uint8_t)zmk_wpm_get_state(),
        .layer = (uint8_t)zmk_keymap_highest_layer_active(),
    };
}

// Both subscriptions go through the display listener, which marshals onto the
// display work queue. That is not a formality: a raw ZMK_LISTENER runs on
// whichever thread raised the event, and touching LVGL from there races the
// display thread already inside it.
ZMK_DISPLAY_WIDGET_LISTENER(widget_background, struct bg_state, bg_update_cb, bg_get_state)
ZMK_SUBSCRIPTION(widget_background, zmk_wpm_state_changed);
ZMK_SUBSCRIPTION(widget_background, zmk_layer_state_changed);

// Scatters and starts whichever set of marks the effect uses.
static void show_effect(struct zmk_widget_background *widget, uint8_t effect) {
    const int32_t w = lv_display_get_horizontal_resolution(NULL);
    const int32_t h = lv_display_get_vertical_resolution(NULL);

    for (int i = 0; i < BG_SPARKLES; i++) {
        quiet(widget->sparkle[i]);
    }
    for (int i = 0; i < BG_ANGER_MARKS; i++) {
        for (int j = 0; j < BG_ANGER_ARCS; j++) {
            quiet(widget->anger[i][j]);
        }
    }
    for (int i = 0; i < BG_SYMBOLS; i++) {
        quiet(widget->symbol[i]);
    }

    if (effect == BG_EFFECT_SPARKLE) {
        for (int i = 0; i < BG_SPARKLES; i++) {
            const int32_t r = (int32_t)rnd_between(BG_R_MIN, BG_R_MAX);
            lv_obj_t *o = widget->sparkle[i];

            build_sparkle(widget->pts[i], r, (BG_LINE_W(r) + 1) / 2);
            lv_line_set_points(o, widget->pts[i], BG_SPARKLE_PTS);
            lv_obj_set_size(o, 2 * r + 1, 2 * r + 1);
            lv_obj_set_style_line_width(o, BG_LINE_W(r), LV_PART_MAIN);
            // The top-left corner is what is chosen, not the centre. Picking a
            // centre and subtracting the radius lets a mark near an edge start
            // at a negative coordinate, and the parent clips whatever falls
            // outside it - which is most of why these came back cut.
            lv_obj_set_pos(o, (int32_t)rnd_between(BG_MARGIN, w - BG_MARGIN - 2 * r),
                           (int32_t)rnd_between(BG_MARGIN, h - BG_MARGIN - 2 * r));
            pulse(o, BG_PEAK_OPA, BG_BURST_MS - BG_IN_MS, rnd_between(0, BG_BURST_MS - BG_IN_MS));
        }
    } else if (effect == BG_EFFECT_ANGER) {
        for (int i = 0; i < BG_ANGER_MARKS; i++) {
            const int32_t r = (int32_t)rnd_between(BG_ANGER_R_MIN, BG_ANGER_R_MAX);
            // Top-left, so the whole 2r box stays inside the parent. Choosing
            // a centre and subtracting r put marks near an edge at negative
            // coordinates, where the parent simply cut them off.
            const int32_t x = (int32_t)rnd_between(BG_MARGIN, w - BG_MARGIN - 2 * r);
            const int32_t y = (int32_t)rnd_between(BG_MARGIN, h - BG_MARGIN - 2 * r);
            // One delay for the whole mark: its four strokes are one symbol and
            // have to breathe together, or it reads as four things flickering.
            const uint32_t delay = rnd_between(0, BG_PULSE_MS);

            for (int j = 0; j < BG_ANGER_ARCS; j++) {
                lv_obj_t *o = widget->anger[i][j];

                build_anger_arc(widget->anger_pts[i][j], r, j, (BG_ANGER_LINE_W(r) + 1) / 2);
                lv_line_set_points(o, widget->anger_pts[i][j], BG_ANGER_ARC_PTS);
                lv_obj_set_size(o, 2 * r + 1, 2 * r + 1);
                lv_obj_set_style_line_width(o, BG_ANGER_LINE_W(r), LV_PART_MAIN);
                lv_obj_set_pos(o, x, y);
                pulse(o, BG_ANGER_MAX_OPA, BG_PULSE_MS, delay);
            }
        }
    } else if (effect == BG_EFFECT_SYMBOLS) {
        for (int i = 0; i < BG_SYMBOLS; i++) {
            lv_obj_t *o = widget->symbol[i];

            // One size only: these are label glyphs rather than traced shapes,
            // so varying them would mean another font rather than another
            // number. Position, character and phase carry the variety instead.
            lv_label_set_text(o,
                              BG_SYMBOL_CHARS[rnd_between(0, ARRAY_SIZE(BG_SYMBOL_CHARS) - 1)]);
            // Measured from the font rather than guessed, so a glyph cannot
            // hang off an edge and be clipped by the screen.
            const int32_t gh = lv_font_get_line_height(&Fredoka_SemiBold_40);
            lv_obj_set_pos(o, (int32_t)rnd_between(BG_MARGIN, w - BG_MARGIN - gh),
                           (int32_t)rnd_between(BG_MARGIN, h - BG_MARGIN - gh));
            pulse(o, BG_SYMBOL_MAX_OPA, BG_PULSE_MS, rnd_between(0, BG_PULSE_MS));
        }
    }
}

static void hide_effect(struct zmk_widget_background *widget) {
    show_effect(widget, BG_EFFECT_NONE);
}

// What is on screen, which is not the same as what the layer is asking for -
// an effect outlives its layer by design.
static uint8_t effect_shown;
static uint32_t effect_started;
static lv_timer_t *effect_timer;

static void effect_timer_cb(lv_timer_t *timer) {
    lv_timer_pause(timer);

    struct zmk_widget_background *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { hide_effect(widget); }
    effect_shown = BG_EFFECT_NONE;
}

static void bg_set_effect(uint8_t effect) {
    if (effect != BG_EFFECT_NONE) {
        // Any pending takedown is cancelled, including one for this same
        // effect - going back to a layer inside its own tail simply continues.
        if (effect_timer) {
            lv_timer_pause(effect_timer);
        }
        if (effect == effect_shown) {
            return;
        }

        struct zmk_widget_background *widget;
        SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { show_effect(widget, effect); }
        effect_shown = effect;
        effect_started = lv_tick_get();
        return;
    }

    if (effect_shown == BG_EFFECT_NONE || !effect_timer) {
        return;
    }

    // The layer is gone but the effect is not, yet. It runs until it has had
    // the full burst, so a tap reads the same as any other burst rather than
    // as a flicker; a hold has already outlived it and goes now.
    const uint32_t elapsed = lv_tick_elaps(effect_started);
    lv_timer_set_period(effect_timer, elapsed >= BG_BURST_MS ? 1 : BG_BURST_MS - elapsed);
    lv_timer_reset(effect_timer);
    lv_timer_resume(effect_timer);
}

void zmk_widget_background_sparkle_burst(struct zmk_widget_background *widget) {
    ARG_UNUSED(widget);

    // Power-up is just a tap that nobody made: raise it and release it at once,
    // and the takedown rule gives it exactly one burst.
    bg_set_effect(BG_EFFECT_SPARKLE);
    bg_set_effect(BG_EFFECT_NONE);
}



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

    // Last, so the marks sit over the rest of this layer. They are still
    // behind the eyes, which is the point - the face shouts, this is the air
    // around it.
    for (int i = 0; i < BG_ANGER_MARKS; i++) {
        for (int j = 0; j < BG_ANGER_ARCS; j++) {
            lv_obj_t *o = lv_line_create(widget->obj);
            widget->anger[i][j] = o;

            lv_obj_remove_style_all(o);
            lv_obj_set_style_line_color(o, lv_color_hex(BG_ANGER_COLOR), LV_PART_MAIN);
            lv_obj_set_style_line_rounded(o, true, LV_PART_MAIN);
            lv_obj_set_style_pad_all(o, 0, LV_PART_MAIN);
            lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
        }
    }

    for (int i = 0; i < BG_SYMBOLS; i++) {
        lv_obj_t *o = lv_label_create(widget->obj);
        widget->symbol[i] = o;

        lv_obj_set_style_text_font(o, &Fredoka_SemiBold_40, LV_PART_MAIN);
        lv_obj_set_style_text_color(o, lv_color_hex(BG_SYMBOL_COLOR), LV_PART_MAIN);
        lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }

    // Infinite and paused; it is given a period each time an effect is released
    // and pauses itself once it has fired. A repeat count of one would have
    // LVGL delete it the first time it ran.
    effect_timer = lv_timer_create(effect_timer_cb, BG_BURST_MS, NULL);
    lv_timer_set_repeat_count(effect_timer, -1);
    lv_timer_pause(effect_timer);

    sys_slist_append(&widgets, &widget->node);

    widget_background_init();

    return 0;
}

lv_obj_t *zmk_widget_background_obj(struct zmk_widget_background *widget) { return widget->obj; }
