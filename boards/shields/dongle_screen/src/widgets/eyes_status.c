/*
 * A pair of animated eyes in place of the layer label.
 *
 * Rendered as two white shapes on black - there is no sclera, so an eye is
 * just its pupil. Every expression is either a rounded bar (size + offset) or
 * a two-segment line (chevron), which keeps the whole vocabulary to two object
 * types per eye.
 *
 * On any layer other than base the expression reports the layer, because
 * knowing where you are beats personality. On the base layer the eyes are
 * driven by activity and typing speed instead.
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/activity.h>
#include <zmk/display.h>
#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/wpm_state_changed.h>
#include <zmk/keymap.h>
#include <zmk/wpm.h>

#include "eyes_status.h"

#define EYES_W 200
#define EYES_H 100

#define EYE_W 56
#define EYE_H 76
#define EYE_R 24
#define EYE_DX 40 // each eye sits this far from centre

#define LINE_W 14
// Rounded line caps extend LINE_W/2 past each endpoint. Insetting by that much
// makes a chevron occupy exactly the same box as a bar, so the eyes don't
// lurch outward when the expression changes.
#define LINE_INSET (LINE_W / 2)

#define LID_CLOSED_H 6
#define BLINK_CLOSE_MS 70
#define BLINK_OPEN_MS 110
#define BLINK_MIN_MS 2600
#define BLINK_MAX_MS 6400

// A saccade is a snap, not a drift: slow interpolation reads as the eyes
// sliding around the panel rather than looking at something.
#define SACCADE_MS 120
#define GLANCE_MIN_MS 1400
#define GLANCE_MAX_MS 4200
#define GAZE_X_MAX 14
#define GAZE_Y_MAX 6

// ZMK's WPM is a rolling estimate and bounces around the boundary, so the
// trigger and the release are deliberately separated.
#define WPM_EXCITED_ON 50
#define WPM_EXCITED_OFF 42

enum eye_shape { SHAPE_BAR, SHAPE_CHEVRON_UP, SHAPE_CHEVRON_IN };

enum expr_id {
    EXPR_NEUTRAL = 0,
    EXPR_SQUEEZED,
    EXPR_WIDE,
    EXPR_DEADPAN,
    EXPR_SHOCK,
    EXPR_SLEEPY,
    EXPR_HAPPY,
    EXPR_COUNT,
};

struct expression {
    enum eye_shape shape;
    int16_t w;
    int16_t h;
    int16_t dy;
    int16_t radius;
    bool wander;
};

static const struct expression expressions[EXPR_COUNT] = {
    [EXPR_NEUTRAL] = {SHAPE_BAR, EYE_W, EYE_H, 0, EYE_R, true},
    [EXPR_SQUEEZED] = {SHAPE_CHEVRON_IN, EYE_W, EYE_H, 0, 0, false},
    [EXPR_WIDE] = {SHAPE_BAR, 60, 88, 0, 26, true},
    [EXPR_DEADPAN] = {SHAPE_BAR, 60, 14, 0, 7, false},
    [EXPR_SHOCK] = {SHAPE_BAR, 24, 24, 0, 12, false},
    [EXPR_SLEEPY] = {SHAPE_BAR, EYE_W, 22, 16, 11, false},
    [EXPR_HAPPY] = {SHAPE_CHEVRON_UP, EYE_W, EYE_H, 0, 0, false},
};

// Layer 0 is handled separately - it is the only layer where the eyes are free
// to express activity rather than state.
static const enum expr_id layer_expr[] = {
    [0] = EXPR_NEUTRAL, [1] = EXPR_HAPPY,   [2] = EXPR_WIDE,
    [3] = EXPR_DEADPAN, [4] = EXPR_SHOCK,
};

static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);

static bool wpm_excited;

static uint32_t rng_state;

static uint32_t rnd(void) {
    if (rng_state == 0) {
        rng_state = (uint32_t)k_uptime_get() | 1u;
    }
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static int32_t rnd_range(int32_t lo, int32_t hi) {
    if (hi <= lo) {
        return lo;
    }
    return lo + (int32_t)(rnd() % (uint32_t)(hi - lo + 1));
}

static void set_chevron_points(struct zmk_widget_eyes_status *widget, int eye,
                               enum eye_shape shape) {
    const int32_t w = EYE_W;
    const int32_t h = EYE_H;
    lv_point_precise_t *p = widget->pts[eye];

    if (shape == SHAPE_CHEVRON_UP) {
        p[0] = (lv_point_precise_t){LINE_INSET, h - LINE_INSET};
        p[1] = (lv_point_precise_t){w / 2, LINE_INSET};
        p[2] = (lv_point_precise_t){w - LINE_INSET, h - LINE_INSET};
    } else {
        // Left eye points right, right eye points left, so the pair squeezes inward.
        bool point_right = (eye == 0);
        lv_coord_t apex = point_right ? (w - LINE_INSET) : LINE_INSET;
        lv_coord_t open = point_right ? LINE_INSET : (w - LINE_INSET);

        p[0] = (lv_point_precise_t){open, LINE_INSET};
        p[1] = (lv_point_precise_t){apex, h / 2};
        p[2] = (lv_point_precise_t){open, h - LINE_INSET};
    }

    lv_line_set_points(widget->line[eye], p, 3);
}

static void apply_geometry(struct zmk_widget_eyes_status *widget) {
    const struct expression *e = &expressions[widget->expr];
    bool use_line = (e->shape != SHAPE_BAR);

    for (int i = 0; i < 2; i++) {
        int16_t dx = (i == 0 ? -EYE_DX : EYE_DX) + widget->gaze_x;
        int16_t dy = e->dy + widget->gaze_y;

        if (use_line) {
            lv_obj_add_flag(widget->bar[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(widget->line[i], LV_OBJ_FLAG_HIDDEN);

            set_chevron_points(widget, i, e->shape);
            lv_obj_set_size(widget->line[i], EYE_W, EYE_H);
            lv_obj_align(widget->line[i], LV_ALIGN_CENTER, dx - EYE_W / 2, dy);
        } else {
            lv_obj_add_flag(widget->line[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(widget->bar[i], LV_OBJ_FLAG_HIDDEN);

            lv_obj_set_size(widget->bar[i], e->w, widget->cur_h);
            lv_obj_set_style_radius(widget->bar[i], e->radius, LV_PART_MAIN);
            lv_obj_align(widget->bar[i], LV_ALIGN_CENTER, dx, dy);
        }
    }
}

static void height_anim_cb(void *var, int32_t v) {
    struct zmk_widget_eyes_status *widget = var;
    widget->cur_h = (int16_t)v;
    apply_geometry(widget);
}

static void gaze_anim_cb(void *var, int32_t v) {
    struct zmk_widget_eyes_status *widget = var;
    widget->gaze_x = (int16_t)v;
    apply_geometry(widget);
}

static void animate(struct zmk_widget_eyes_status *widget, lv_anim_exec_xcb_t cb, int32_t from,
                    int32_t to, uint32_t ms, uint32_t delay, lv_anim_path_cb_t path) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, widget);
    lv_anim_set_exec_cb(&a, cb);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_time(&a, ms);
    lv_anim_set_delay(&a, delay);
    lv_anim_set_path_cb(&a, path);
    lv_anim_start(&a);
}

static void blink_timer_cb(lv_timer_t *timer) {
    struct zmk_widget_eyes_status *widget = lv_timer_get_user_data(timer);
    const struct expression *e = &expressions[widget->expr];

    // Chevrons and already-flat expressions have nothing to close.
    if (e->shape == SHAPE_BAR && e->h > LID_CLOSED_H + 10) {
        animate(widget, height_anim_cb, widget->cur_h, LID_CLOSED_H, BLINK_CLOSE_MS, 0,
                lv_anim_path_ease_in);
        animate(widget, height_anim_cb, LID_CLOSED_H, e->h, BLINK_OPEN_MS, BLINK_CLOSE_MS,
                lv_anim_path_ease_out);
    }

    lv_timer_set_period(timer, rnd_range(BLINK_MIN_MS, BLINK_MAX_MS));
}

static void glance_timer_cb(lv_timer_t *timer) {
    struct zmk_widget_eyes_status *widget = lv_timer_get_user_data(timer);
    const struct expression *e = &expressions[widget->expr];

    if (e->wander) {
        int16_t target = (int16_t)rnd_range(-GAZE_X_MAX, GAZE_X_MAX);
        widget->gaze_y = (int16_t)rnd_range(-GAZE_Y_MAX, GAZE_Y_MAX);
        animate(widget, gaze_anim_cb, widget->gaze_x, target, SACCADE_MS, 0,
                lv_anim_path_ease_out);
    }

    lv_timer_set_period(timer, rnd_range(GLANCE_MIN_MS, GLANCE_MAX_MS));
}

static void set_expression(struct zmk_widget_eyes_status *widget, enum expr_id id) {
    if (widget->expr == id) {
        return;
    }

    const struct expression *e = &expressions[id];
    widget->expr = id;

    // A blink mid-change would fight the new height.
    lv_anim_delete(widget, height_anim_cb);

    if (!e->wander && (widget->gaze_x || widget->gaze_y)) {
        lv_anim_delete(widget, gaze_anim_cb);
        widget->gaze_x = 0;
        widget->gaze_y = 0;
    }

    if (e->shape == SHAPE_BAR) {
        animate(widget, height_anim_cb, widget->cur_h, e->h, SACCADE_MS, 0,
                lv_anim_path_ease_out);
    } else {
        widget->cur_h = e->h;
    }

    apply_geometry(widget);
}

struct eyes_state {
    uint8_t layer;
    uint8_t wpm;
    bool idle;
};

static enum expr_id resolve(struct eyes_state state) {
    if (state.layer != 0) {
        if (state.layer < ARRAY_SIZE(layer_expr)) {
            return layer_expr[state.layer];
        }
        return EXPR_NEUTRAL;
    }

    if (state.idle) {
        return EXPR_SLEEPY;
    }

    if (wpm_excited) {
        wpm_excited = (state.wpm > WPM_EXCITED_OFF);
    } else {
        wpm_excited = (state.wpm >= WPM_EXCITED_ON);
    }

    // Squeezed reads as effort, which is what typing fast actually feels like.
    return wpm_excited ? EXPR_SQUEEZED : EXPR_NEUTRAL;
}

static void eyes_update_cb(struct eyes_state state) {
    struct zmk_widget_eyes_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
        widget->idle = state.idle;
        set_expression(widget, resolve(state));
    }
}

static struct eyes_state eyes_get_state(const zmk_event_t *eh) {
    ARG_UNUSED(eh);
    return (struct eyes_state){
        .layer = zmk_keymap_highest_layer_active(),
        .wpm = zmk_wpm_get_state(),
        .idle = (zmk_activity_get_state() != ZMK_ACTIVITY_ACTIVE),
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_eyes_status, struct eyes_state, eyes_update_cb, eyes_get_state)
ZMK_SUBSCRIPTION(widget_eyes_status, zmk_layer_state_changed);
ZMK_SUBSCRIPTION(widget_eyes_status, zmk_wpm_state_changed);
ZMK_SUBSCRIPTION(widget_eyes_status, zmk_activity_state_changed);

int zmk_widget_eyes_status_init(struct zmk_widget_eyes_status *widget, lv_obj_t *parent) {
    widget->obj = lv_obj_create(parent);
    lv_obj_remove_style_all(widget->obj);
    lv_obj_set_size(widget->obj, EYES_W, EYES_H);
    lv_obj_remove_flag(widget->obj, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < 2; i++) {
        widget->bar[i] = lv_obj_create(widget->obj);
        lv_obj_remove_style_all(widget->bar[i]);
        lv_obj_set_style_bg_color(widget->bar[i], lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(widget->bar[i], LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_remove_flag(widget->bar[i], LV_OBJ_FLAG_SCROLLABLE);

        widget->line[i] = lv_line_create(widget->obj);
        lv_obj_set_style_line_color(widget->line[i], lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_line_width(widget->line[i], LINE_W, LV_PART_MAIN);
        lv_obj_set_style_line_rounded(widget->line[i], true, LV_PART_MAIN);
        lv_obj_add_flag(widget->line[i], LV_OBJ_FLAG_HIDDEN);
    }

    widget->expr = EXPR_NEUTRAL;
    widget->cur_h = EYE_H;
    widget->gaze_x = 0;
    widget->gaze_y = 0;
    widget->idle = false;
    apply_geometry(widget);

    lv_timer_t *blink =
        lv_timer_create(blink_timer_cb, rnd_range(BLINK_MIN_MS, BLINK_MAX_MS), widget);
    lv_timer_set_repeat_count(blink, -1);

    lv_timer_t *glance =
        lv_timer_create(glance_timer_cb, rnd_range(GLANCE_MIN_MS, GLANCE_MAX_MS), widget);
    lv_timer_set_repeat_count(glance, -1);

    sys_slist_append(&widgets, &widget->node);

    widget_eyes_status_init();
    return 0;
}

lv_obj_t *zmk_widget_eyes_status_obj(struct zmk_widget_eyes_status *widget) { return widget->obj; }
