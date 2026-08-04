/*
 * A pair of animated eyes in place of the layer label.
 *
 * Rendered as two white shapes on black - there is no sclera, so an eye is
 * just its pupil. Every expression is either a rounded bar (size, offset and
 * radius) or an lv_line polyline, which keeps the whole vocabulary to two
 * object types per eye, plus a canvas beneath for the two shapes that need a
 * solid interior. Those two are cut out of the neutral shape rather than
 * drawn independently, so the whole set shares one silhouette.
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
#include <fonts.h>

#define EYES_W 220
#define EYES_H 120

#define EYE_W 56
#define EYE_H 76
#define EYE_R 24
// Each eye sits this far from centre, so the pair is 2x this apart. At 40 a
// 56px-wide eye left only 24px between them, which read as crowded.
#define EYE_DX 46

// Default stroke for line shapes. Expressions can override it via line_w.
#define LINE_W 14

// lv_trigo_sin returns a sine scaled to this. Spelled out rather than using
// LV_TRIGO_SIN_MAX so this doesn't depend on that macro's name.
#define TRIG_MAX 32767

// Vertical extent is scaled by `openness` rather than set directly, so blinks
// and expression changes share one mechanism and work on every shape.
#define OPEN_FULL 256
#define OPEN_SHUT 12

// A full frame at 12.5fps. Anything shorter can fall entirely between two
// redraws, so the blink would be visible only some of the time - animations
// keep stepping every 10ms, but the screen only samples them every 80.
#define BLINK_CLOSE_MS 80
#define BLINK_OPEN_MS 110
// Roughly a third as often as before. At 2.6-6.4s it caught the eye
// constantly, which is the opposite of what a resting face should do.
#define BLINK_MIN_MS 7000
#define BLINK_MAX_MS 16000

// Expression changes blink through the swap. Cutting straight from one shape
// to another is a hard pop; hiding it behind a lid reads as intentional.
#define MORPH_CLOSE_MS 80
#define MORPH_OPEN_MS 120

// A saccade is a snap, not a drift: slow interpolation reads as the eyes
// sliding around the panel rather than looking at something.
#define SACCADE_MS 120
#define GLANCE_MIN_MS 1400
#define GLANCE_MAX_MS 4200
#define GAZE_X_MAX 14
#define GAZE_Y_MAX 6

// ZMK's WPM is a rolling estimate and bounces, so each threshold releases
// well below where it triggers.
#define WPM_SQUEEZE_ON 40
#define WPM_SQUEEZE_OFF 33
#define WPM_CONFUSED_ON 70
#define WPM_CONFUSED_OFF 62

// Squeezing is an effort, so it pulses rather than sitting still. STRAIN_MIN
// is how far shut it gets at the bottom of the pulse, out of OPEN_FULL - a
// shallower dip than before, and quicker, so it reads as a tremor under load
// rather than as the eyes repeatedly closing.
#define STRAIN_MS 260
#define STRAIN_MIN 216

// On top of the strain pulse, a squeeze shudders sideways: a short fast burst
// then a long pause, rather than a constant tremble. The animation runs a
// counter across the whole period and only produces movement in the first
// SHAKE_ACTIVE of it, which is how the pause is achieved with one anim.
#define SHAKE_PERIOD 1000
#define SHAKE_ACTIVE 170
#define SHAKE_PERIOD_MS 2200
#define SHAKE_CYCLES 3
#define SHAKE_PX 3

// Proportioned off a reference frame of the manga dizzy-spiral: two full
// turns, and a stroke about as wide as the gap between turns. At 1.5 turns
// with the default LINE_W the turns sat exactly one stroke apart and merged
// into a solid disc; at 6px over 1.5 turns it went wispy. Two turns over a
// 25px radius puts the coils 12.5px apart, so a 6px stroke leaves 6.5px of
// gap - very close to the reference's 1:1.
#define SPIRAL_PTS 28
#define SPIRAL_TURNS 720 // degrees swept from centre to rim
// 1.5s whipped the free outer end round like a fan blade; 3.5s was sedate.
#define SPIN_MS 2400

// The drift has its own slow driver rather than being derived from `spin`.
// Dividing spin down would have snapped every time it wrapped 359 -> 0, since
// half of a wrap is a 180 degree jump.
#define WOBBLE_MS 7000
#define WOBBLE_PX 2

// ZMK only reports ACTIVE and IDLE here (ZMK_SLEEP is off), so the deeper
// "actually asleep" stage is timed locally.
#define ZZZ_DELAY_MS 20000
#define ZZZ_CYCLE_MS 1800
#define ZZZ_RISE 16

enum eye_shape {
    SHAPE_BAR,
    SHAPE_CHEVRON_IN,
    SHAPE_ARC_DOWN,
    SHAPE_LIDDED,
    SHAPE_ANGRY,
    SHAPE_SPIRAL,
};

// Points per corner when tracing a rounded rectangle. Both derived shapes
// start from neutral's outline, so they share its silhouette by construction
// rather than by eye.
#define RR_CORNER_PTS 5

// Angry is neutral sliced by a line running from high on the outer edge to low
// on the inner one, keeping what falls below. Percentages of the eye's height.
#define ANGRY_CUT_OUTER_PCT 42
#define ANGRY_CUT_INNER_PCT 78

// Unamused is neutral's lower half with a short tail off the top edge.
#define LID_TAIL 14

// Points along a shallow curve. A 3-point chevron would read as a hard V;
// sampling a sine gives it an actual bow.
#define ARC_PTS 7

// Canvas behind the outline, for shapes that need a solid interior. lv_line
// only strokes, and widening the stroke until it closes a shape destroys the
// detail that defined it.
//
// Must cover the whole box of the largest filled shape, not just the part with
// ink in it: the polygon is placed by centring its box in the canvas, so a box
// taller than the canvas would be offset off the top. Unamused is the largest
// at EYE_W + LID_TAIL by EYE_H. Each pixel costs 3 bytes, twice.
#define EYE_FILL_W (EYE_W + LID_TAIL + 4)
#define EYE_FILL_H (EYE_H + 4)
// A scanline crosses at most this many edges. Nine-point shapes need far less.
#define FILL_MAX_X 12

// lv_color_t rather than a packed byte array, matching how battery_status.c
// backs its canvas. Over-allocates against RGB565 but is the proven pattern.
static lv_color_t eye_fill_buf[2][EYE_FILL_W * EYE_FILL_H];

enum expr_id {
    // layer_expr only, never an actual expression: this layer has none, so the
    // eyes keep doing whatever they were already doing. Zero so that any layer
    // left out of the table gets this behaviour by default.
    EXPR_NONE = 0,
    EXPR_NEUTRAL,
    EXPR_SQUEEZED,
    EXPR_SHOCK,
    EXPR_SLEEPY,
    EXPR_CONFUSED,
    EXPR_UNAMUSED,
    EXPR_ANGRY,
    EXPR_COUNT,
};

struct expression {
    enum eye_shape shape;
    int16_t w;
    int16_t h;
    int16_t dx; // fixed horizontal bias, for looking off to one side
    int16_t dy;
    int16_t radius;
    bool wander;
    int16_t line_w; // stroke width for line shapes; 0 means LINE_W
    int16_t spread; // extra separation, pushing each eye outward
    int16_t rot;      // tilt in 0.1 degrees, mirrored between the eyes
    bool filled;      // line shapes only: fill the traced path solid
    int16_t morph_ms; // total time to blink into this expression; 0 = default
};

static const struct expression expressions[EXPR_COUNT] = {
    // The resting face. Both derived shapes below are cut out of this one, so
    // changing it changes them.
    [EXPR_NEUTRAL] = {SHAPE_BAR, EYE_W, EYE_H, 0, 0, EYE_R, true},
    [EXPR_SQUEEZED] = {SHAPE_CHEVRON_IN, EYE_W, EYE_H, 0, 0, 0, false},
    [EXPR_SHOCK] = {SHAPE_BAR, 24, 24, 0, 0, 12, false},
    // Box height sets the bow: depth is h minus the stroke.
    [EXPR_SLEEPY] = {SHAPE_ARC_DOWN, EYE_W, 30, 0, 12, 0, false},
    // Neutral's own outline, cut. Their boxes are neutral's size plus whatever
    // the cut needs - the lid's tail, and nothing extra for angry. Unamused
    // spreads slightly, since the tail eats into the gap between the eyes.
    //
    // Both morph in a single frame. These are the layer expressions, so they
    // want to land the instant the key goes down; the default 200ms is fine
    // for a mood drifting in, but reads as lag when it is answering a keypress.
    [EXPR_UNAMUSED] = {SHAPE_LIDDED, EYE_W + LID_TAIL, EYE_H / 2, 0, 0, 0, false, 9, 6, 0, true,
                       80},
    [EXPR_ANGRY] = {SHAPE_ANGRY, EYE_W, EYE_H, 0, 0, 0, false, 9, 0, 0, true, 80},
    // Wider box means wider coil spacing, so the stroke goes up with it to
    // hold the reference's 1:1 stroke-to-gap.
    // Deliberately excluded from the widening. EYE_DX + 6 puts these 52px from
    // centre, exactly where they sat before it - at 86px per eye they were
    // already far enough apart, and any wider reaches the case lip.
    [EXPR_CONFUSED] = {SHAPE_SPIRAL, 86, 86, 0, 0, 0, false, 10, 6},
};

// Layer 0 is handled separately - it is the only layer where the eyes are free
// to express activity rather than state.
static const enum expr_id layer_expr[] = {
    // Layer 0's entry is never read - the base layer is handled before this
    // table is consulted. sym is EXPR_NONE, so holding it changes nothing:
    // whatever the eyes were showing carries on.
    [0] = EXPR_NONE,  [1] = EXPR_NONE,  [2] = EXPR_UNAMUSED,
    [3] = EXPR_ANGRY, [4] = EXPR_SHOCK,
};

static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);

static uint8_t wpm_level; // 0 none, 1 squeezed, 2 confused
static lv_timer_t *zzz_timer;

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

static int32_t scaled(int32_t v, int32_t openness) { return (v * openness) / OPEN_FULL; }

static int32_t sin_of(int32_t deg) {
    while (deg < 0) {
        deg += 360;
    }
    return lv_trigo_sin((int16_t)(deg % 360));
}

static int32_t cos_of(int32_t deg) { return sin_of(deg + 90); }

static void set_chevron_points(struct zmk_widget_eyes_status *widget, int eye,
                               enum eye_shape shape, int32_t w, int32_t box_h, int32_t strain,
                               int32_t inset) {
    int32_t h = scaled(box_h, widget->openness);

    // A squeeze shuts the eye vertically - the open ends come together, the
    // way a real one scrunches. Driving the apex in and out horizontally
    // instead just looks like the point twitching.
    if (shape == SHAPE_CHEVRON_IN) {
        int32_t k = STRAIN_MIN + ((OPEN_FULL - STRAIN_MIN) * strain) / OPEN_FULL;
        h = (h * k) / OPEN_FULL;
    }

    const int32_t top = (box_h - h) / 2; // keep the shape vertically centred as it closes
    lv_point_precise_t *p = widget->pts[eye];

    // Left eye points right, right eye points left, so the pair squeezes
    // inward at each other.
    bool point_right = (eye == 0);
    int32_t apex = point_right ? (w - inset) : inset;
    int32_t open = point_right ? inset : (w - inset);

    p[0] = (lv_point_precise_t){open, top + inset};
    p[1] = (lv_point_precise_t){apex, top + h / 2};
    p[2] = (lv_point_precise_t){open, top + h - inset};

    lv_line_set_points(widget->line[eye], p, 3);
}

// Bows downward: ends high, middle low. Reads as eyes closed and settled,
// where a flat bar reads as merely narrowed.
static void set_arc_points(struct zmk_widget_eyes_status *widget, int eye, int32_t w,
                           int32_t box_h, int32_t inset) {
    const int32_t h = scaled(box_h, widget->openness);
    const int32_t top = (box_h - h) / 2;
    const int32_t span = w - 2 * inset;
    const int32_t depth = h - 2 * inset;
    lv_point_precise_t *p = widget->pts[eye];

    for (int i = 0; i < ARC_PTS; i++) {
        // sin across 0..180 degrees: zero at both ends, deepest in the middle.
        int32_t deg = (180 * i) / (ARC_PTS - 1);
        p[i].x = inset + (span * i) / (ARC_PTS - 1);
        p[i].y = top + inset + (depth * sin_of(deg)) / TRIG_MAX;
    }

    lv_line_set_points(widget->line[eye], p, ARC_PTS);
}


// Traces a rounded rectangle clockwise from its top-left corner. Neutral's
// silhouette, which the derived expressions are cut out of.
static int rounded_rect(lv_point_precise_t *p, int32_t x0, int32_t y0, int32_t w, int32_t h,
                        int32_t r) {
    if (r > w / 2) {
        r = w / 2;
    }
    if (r > h / 2) {
        r = h / 2;
    }

    const int32_t cx[4] = {x0 + r, x0 + w - r, x0 + w - r, x0 + r};
    const int32_t cy[4] = {y0 + r, y0 + r, y0 + h - r, y0 + h - r};
    const int32_t a0[4] = {180, 270, 0, 90};
    int n = 0;

    for (int k = 0; k < 4; k++) {
        for (int i = 0; i < RR_CORNER_PTS; i++) {
            int32_t deg = a0[k] + (90 * i) / (RR_CORNER_PTS - 1);
            p[n].x = cx[k] + (r * cos_of(deg)) / TRIG_MAX;
            p[n].y = cy[k] + (r * sin_of(deg)) / TRIG_MAX;
            n++;
        }
    }

    return n;
}

// Sutherland-Hodgman against a single half-plane: keeps whatever lies below
// the line running from (x0, ya) to (x0 + w, yb).
static int clip_below(lv_point_precise_t *dst, const lv_point_precise_t *src, int n, int32_t x0,
                      int32_t w, int32_t ya, int32_t yb) {
    int m = 0;

    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        int32_t xi = (int32_t)src[i].x, yi = (int32_t)src[i].y;
        int32_t xj = (int32_t)src[j].x, yj = (int32_t)src[j].y;

        int32_t di = yi - (ya + ((yb - ya) * (xi - x0)) / w);
        int32_t dj = yj - (ya + ((yb - ya) * (xj - x0)) / w);

        if (di >= 0) {
            dst[m++] = src[i];
        }

        if ((di >= 0) != (dj >= 0)) {
            int32_t den = di - dj;
            if (den == 0) {
                den = 1;
            }
            dst[m].x = xi + ((xj - xi) * di) / den;
            dst[m].y = yi + ((yj - yi) * di) / den;
            m++;
        }
    }

    return m;
}

// Neutral's lower half, with a short tail running off the top edge. Built from
// the same rounded rectangle so the curve matches neutral exactly rather than
// approximating it.
static int set_lid_points(struct zmk_widget_eyes_status *widget, int eye, int32_t w,
                          int32_t box_h, int32_t inset) {
    const int32_t h = scaled(box_h, widget->openness);
    const int32_t top = (box_h - h) / 2;
    const int32_t eye_w = w - LID_TAIL - 2 * inset;
    const int32_t x0 = inset;

    // What survives the cut, which is half of the eye this is derived from.
    // The box is therefore half of neutral's height, and the notional full
    // eye - twice this - is hung above the cut line so its bottom half lands
    // inside the box.
    const int32_t bowl_h = h - 2 * inset;

    lv_point_precise_t rr[EYE_MAX_PTS];
    lv_point_precise_t half[EYE_MAX_PTS];
    lv_point_precise_t *p = widget->pts[eye];

    int n = rounded_rect(rr, x0, top + inset - bowl_h, eye_w, 2 * bowl_h, EYE_R);
    n = clip_below(half, rr, n, x0, eye_w, top + inset, top + inset);

    // Tail first, so the stroke lays the top edge down before rounding the
    // bowl. The closing segment back to it has no area, so the fill is the
    // bowl alone.
    p[0].x = x0 + eye_w + LID_TAIL;
    p[0].y = top + inset;

    for (int i = 0; i < n && i + 1 < EYE_MAX_PTS; i++) {
        p[i + 1] = half[i];
    }

    // Repeat the first point so the stroke closes. lv_line draws an open
    // polyline while the fill treats the path as closed, so without this the
    // top edge would be filled but never stroked - a hard flat edge against
    // rounded strokes everywhere else, which reads as a notch cut out of the
    // shape. Closing it also draws the lid across the top of the bowl, which
    // is what the shape wants anyway.
    int total = n + 1;
    if (total < EYE_MAX_PTS) {
        p[total++] = p[0];
    }

    lv_line_set_points(widget->line[eye], p, total);
    return total;
}

// Neutral, sliced by a line running from high on the outer edge to low on the
// inner one, keeping what falls below. Being a cut of the same rounded
// rectangle, the curve of the remaining bottom is neutral's own.
static int set_angry_points(struct zmk_widget_eyes_status *widget, int eye, int32_t w,
                            int32_t box_h, int32_t inset) {
    const int32_t h = scaled(box_h, widget->openness);
    const int32_t top = (box_h - h) / 2 + inset;
    const int32_t eh = h - 2 * inset;
    const int32_t x0 = inset;
    const int32_t ew = w - 2 * inset;

    lv_point_precise_t rr[EYE_MAX_PTS];
    lv_point_precise_t *p = widget->pts[eye];

    int n = rounded_rect(rr, x0, top, ew, eh, EYE_R);

    // Outer edge is the left one on eye 0 and the right one on eye 1, so the
    // cut simply runs the other way rather than mirroring the whole polygon.
    int32_t cut_out = top + (eh * ANGRY_CUT_OUTER_PCT) / 100;
    int32_t cut_in = top + (eh * ANGRY_CUT_INNER_PCT) / 100;
    int32_t ya = eye ? cut_in : cut_out;
    int32_t yb = eye ? cut_out : cut_in;

    n = clip_below(p, rr, n, x0, ew, ya, yb);

    // Closed, for the same reason as the lid: the cut edge is filled, so it
    // has to be stroked too or it reads as a slice taken out of the shape
    // rather than as its top.
    if (n < EYE_MAX_PTS) {
        p[n++] = p[0];
    }

    lv_line_set_points(widget->line[eye], p, n);
    return n;
}

static void set_spiral_points(struct zmk_widget_eyes_status *widget, int eye, int32_t w, int32_t h,
                              int32_t inset) {
    const int32_t cx = w / 2;
    const int32_t cy = h / 2;
    const int32_t r_max = scaled(MIN(w, h) / 2 - inset, widget->openness);
    const int32_t last = SPIRAL_PTS - 1;
    // Both eyes share a phase: same starting orientation, same direction, same
    // rate. Only the drift below is per-eye, so they read as a matched pair
    // that happens to be unsteady rather than as two independent objects.
    const int32_t phase = widget->spin;
    lv_point_precise_t *p = widget->pts[eye];

    for (int i = 0; i < SPIRAL_PTS; i++) {
        int32_t deg = phase + (SPIRAL_TURNS * i) / last;
        int32_t r = (r_max * i) / last;
        p[i].x = cx + (r * cos_of(deg)) / TRIG_MAX;
        p[i].y = cy + (r * sin_of(deg)) / TRIG_MAX;
    }

    lv_line_set_points(widget->line[eye], p, SPIRAL_PTS);
}


// Even-odd scanline fill of the polygon the outline traces. Deliberately not
// anti-aliased: the same points are stroked on top with rounded joins, and
// that stroke both softens the corners and covers the stepped edges this
// leaves behind.
static void fill_polygon(lv_obj_t *canvas, const lv_point_precise_t *p, int n, int32_t ox,
                         int32_t oy) {
    lv_canvas_fill_bg(canvas, lv_color_black(), LV_OPA_COVER);

    for (int32_t y = 0; y < EYE_FILL_H; y++) {
        int32_t xs[FILL_MAX_X];
        int cnt = 0;

        for (int i = 0; i < n && cnt < FILL_MAX_X; i++) {
            int j = (i + 1) % n;
            int32_t y0 = (int32_t)p[i].y + oy;
            int32_t y1 = (int32_t)p[j].y + oy;

            // Half-open test, so a vertex lying exactly on the scanline is
            // counted once rather than twice.
            if ((y0 <= y && y1 > y) || (y1 <= y && y0 > y)) {
                int32_t x0 = (int32_t)p[i].x + ox;
                int32_t x1 = (int32_t)p[j].x + ox;
                xs[cnt++] = x0 + ((y - y0) * (x1 - x0)) / (y1 - y0);
            }
        }

        for (int a = 1; a < cnt; a++) {
            int32_t v = xs[a];
            int b = a - 1;
            while (b >= 0 && xs[b] > v) {
                xs[b + 1] = xs[b];
                b--;
            }
            xs[b + 1] = v;
        }

        for (int k = 0; k + 1 < cnt; k += 2) {
            int32_t from = MAX(xs[k], 0);
            int32_t to = MIN(xs[k + 1], EYE_FILL_W - 1);

            for (int32_t x = from; x <= to; x++) {
                lv_canvas_set_px(canvas, x, y, lv_color_white(), LV_OPA_COVER);
            }
        }
    }
}

static void apply_geometry(struct zmk_widget_eyes_status *widget) {
    const struct expression *e = &expressions[widget->expr];

    const int32_t lw = e->line_w ? e->line_w : LINE_W;
    // Rounded caps extend half the stroke past each endpoint. Insetting by
    // that much makes a line shape occupy the same box as a bar, so the eyes
    // don't lurch outward when the expression changes.
    const int32_t inset = lw / 2;

    for (int i = 0; i < 2; i++) {
        enum eye_shape shape = e->shape;
        int32_t box_h = e->h;
        int16_t out = (int16_t)(EYE_DX + e->spread);
        int16_t dx = (i == 0 ? -out : out) + e->dx + widget->gaze_x;
        int16_t dy = e->dy + widget->gaze_y;

        // Written unconditionally on both objects, not just on the branch that
        // uses it. Setting it only inside the bar branch left a stale tilt on a
        // hidden bar, which reappeared the next time that bar was shown by an
        // expression that never asked to be rotated. Mirrored, so the pair
        // tilts toward each other rather than both leaning the same way.
        int16_t rot = i ? -e->rot : e->rot;
        lv_obj_set_style_transform_rotation(widget->bar[i], rot, LV_PART_MAIN);
        lv_obj_set_style_transform_rotation(widget->line[i], rot, LV_PART_MAIN);
        lv_obj_set_style_transform_pivot_x(widget->line[i], e->w / 2, LV_PART_MAIN);
        lv_obj_set_style_transform_pivot_y(widget->line[i], box_h / 2, LV_PART_MAIN);

        // Same offset for both eyes, deliberately: this one shudders as a
        // pair rather than each eye going its own way.
        if (shape == SHAPE_CHEVRON_IN) {
            dx += widget->shake;
        }

        // Each spiral drifts on its own slow circle, a third of a turn out of
        // phase with the other, so the pair looks unsteady without either one
        // appearing to twitch.
        if (shape == SHAPE_SPIRAL) {
            int32_t phase = widget->wob + (i ? 120 : 0);
            dx += (int16_t)((cos_of(phase) * WOBBLE_PX) / TRIG_MAX);
            dy += (int16_t)((sin_of(phase) * WOBBLE_PX) / TRIG_MAX);
        }

        if (shape == SHAPE_BAR) {
            int32_t h = scaled(box_h, widget->openness);
            if (h < 2) {
                h = 2;
            }

            lv_obj_add_flag(widget->fill[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(widget->line[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(widget->bar[i], LV_OBJ_FLAG_HIDDEN);

            lv_obj_set_size(widget->bar[i], e->w, h);
            lv_obj_set_style_radius(widget->bar[i], e->radius, LV_PART_MAIN);
            lv_obj_set_style_transform_pivot_x(widget->bar[i], e->w / 2, LV_PART_MAIN);
            lv_obj_set_style_transform_pivot_y(widget->bar[i], h / 2, LV_PART_MAIN);
            lv_obj_align(widget->bar[i], LV_ALIGN_CENTER, dx, dy);
            continue;
        }

        lv_obj_add_flag(widget->bar[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(widget->line[i], LV_OBJ_FLAG_HIDDEN);

        int npts;

        switch (shape) {
        case SHAPE_ARC_DOWN:
            set_arc_points(widget, i, e->w, box_h, inset);
            npts = ARC_PTS;
            break;
            break;
        case SHAPE_LIDDED:
            npts = set_lid_points(widget, i, e->w, box_h, inset);
            break;
        case SHAPE_ANGRY:
            npts = set_angry_points(widget, i, e->w, box_h, inset);
            break;
        case SHAPE_SPIRAL:
            set_spiral_points(widget, i, e->w, box_h, inset);
            npts = SPIRAL_PTS;
            break;
            break;
        default:
            set_chevron_points(widget, i, shape, e->w, box_h, widget->strain, inset);
            npts = 3;
            break;
        }

        if (e->filled) {
            // The canvas is larger than the shape's box, so the points are
            // offset to sit centred in it and both objects can then align to
            // the same point.
            fill_polygon(widget->fill[i], widget->pts[i], npts, (EYE_FILL_W - e->w) / 2,
                         (EYE_FILL_H - box_h) / 2);

            lv_obj_remove_flag(widget->fill[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_transform_pivot_x(widget->fill[i], EYE_FILL_W / 2, LV_PART_MAIN);
            lv_obj_set_style_transform_pivot_y(widget->fill[i], EYE_FILL_H / 2, LV_PART_MAIN);
            lv_obj_set_style_transform_rotation(widget->fill[i], rot, LV_PART_MAIN);
            lv_obj_align(widget->fill[i], LV_ALIGN_CENTER, dx, dy);
        } else {
            lv_obj_add_flag(widget->fill[i], LV_OBJ_FLAG_HIDDEN);
        }

        lv_obj_set_style_line_width(widget->line[i], lw, LV_PART_MAIN);
        lv_obj_set_size(widget->line[i], e->w, box_h);
        // Aligns by the object's own centre, exactly like the bars do - the
        // line must not carry an extra half-width offset.
        lv_obj_align(widget->line[i], LV_ALIGN_CENTER, dx, dy);
    }
}

static void openness_anim_cb(void *var, int32_t v) {
    struct zmk_widget_eyes_status *widget = var;
    widget->openness = (int16_t)v;
    apply_geometry(widget);
}

static void strain_anim_cb(void *var, int32_t v) {
    struct zmk_widget_eyes_status *widget = var;
    widget->strain = (int16_t)v;
    apply_geometry(widget);
}

static void spin_anim_cb(void *var, int32_t v) {
    struct zmk_widget_eyes_status *widget = var;
    widget->spin = (int16_t)v;
    apply_geometry(widget);
}

static void wob_anim_cb(void *var, int32_t v) {
    struct zmk_widget_eyes_status *widget = var;
    widget->wob = (int16_t)v;
    apply_geometry(widget);
}

static void shake_anim_cb(void *var, int32_t v) {
    struct zmk_widget_eyes_status *widget = var;
    int32_t off = 0;

    if (v < SHAKE_ACTIVE) {
        int32_t deg = (v * 360 * SHAKE_CYCLES) / SHAKE_ACTIVE;
        off = (sin_of(deg) * SHAKE_PX) / TRIG_MAX;
    }

    widget->shake = (int16_t)off;
    apply_geometry(widget);
}

static void gaze_anim_cb(void *var, int32_t v) {
    struct zmk_widget_eyes_status *widget = var;
    widget->gaze_x = (int16_t)v;
    apply_geometry(widget);
}

static void animate(struct zmk_widget_eyes_status *widget, lv_anim_exec_xcb_t cb, int32_t from,
                    int32_t to, uint32_t ms, uint32_t delay, lv_anim_path_cb_t path,
                    lv_anim_completed_cb_t done) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, widget);
    lv_anim_set_exec_cb(&a, cb);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_time(&a, ms);
    lv_anim_set_delay(&a, delay);
    lv_anim_set_path_cb(&a, path);
    if (done) {
        lv_anim_set_completed_cb(&a, done);
    }
    lv_anim_start(&a);
}

static void loop_anim(struct zmk_widget_eyes_status *widget, lv_anim_exec_xcb_t cb, int32_t from,
                      int32_t to, uint32_t ms, bool playback) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, widget);
    lv_anim_set_exec_cb(&a, cb);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_time(&a, ms);
    if (playback) {
        lv_anim_set_playback_time(&a, ms);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    } else {
        lv_anim_set_path_cb(&a, lv_anim_path_linear);
    }
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
}

// Only one of these should ever be running, so both are cleared on every
// expression change and the incoming one restarted.
static void set_idle_motion(struct zmk_widget_eyes_status *widget) {
    lv_anim_delete(widget, strain_anim_cb);
    lv_anim_delete(widget, spin_anim_cb);
    lv_anim_delete(widget, shake_anim_cb);
    lv_anim_delete(widget, wob_anim_cb);
    widget->strain = OPEN_FULL;
    widget->spin = 0;
    widget->shake = 0;
    widget->wob = 0;

    if (widget->expr == EXPR_SQUEEZED) {
        loop_anim(widget, strain_anim_cb, OPEN_FULL, 0, STRAIN_MS, true);
        loop_anim(widget, shake_anim_cb, 0, SHAKE_PERIOD, SHAKE_PERIOD_MS, false);
    } else if (widget->expr == EXPR_CONFUSED) {
        loop_anim(widget, spin_anim_cb, 0, 359, SPIN_MS, false);
        loop_anim(widget, wob_anim_cb, 0, 359, WOBBLE_MS, false);
    }
}

static void show_zzz(struct zmk_widget_eyes_status *widget, bool show) {
    for (int i = 0; i < 3; i++) {
        if (show) {
            lv_obj_remove_flag(widget->zzz[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(widget->zzz[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void zzz_timer_cb(lv_timer_t *timer) {
    struct zmk_widget_eyes_status *widget = lv_timer_get_user_data(timer);
    if (widget->expr == EXPR_SLEEPY) {
        show_zzz(widget, true);
    }
}

static void morph_open(lv_anim_t *a) {
    struct zmk_widget_eyes_status *widget = a->var;

    // Swapped at the bottom of the blink, where nothing is visible.
    widget->expr = widget->pending_expr;

    const struct expression *e = &expressions[widget->expr];
    if (!e->wander && (widget->gaze_x || widget->gaze_y)) {
        lv_anim_delete(widget, gaze_anim_cb);
        widget->gaze_x = 0;
        widget->gaze_y = 0;
    }

    set_idle_motion(widget);
    apply_geometry(widget);

    uint32_t open_ms = e->morph_ms ? (uint32_t)e->morph_ms / 2 : MORPH_OPEN_MS;
    animate(widget, openness_anim_cb, OPEN_SHUT, OPEN_FULL, open_ms, 0, lv_anim_path_ease_out,
            NULL);
}

static void set_expression(struct zmk_widget_eyes_status *widget, enum expr_id id) {
    // A morph adopts its new expression in the animation's completion callback.
    // Anything that replaces that animation before it finishes - another morph,
    // or a blink landing on the same object and exec_cb - drops the callback,
    // and the widget is left rendering the old expression forever. Adopt any
    // orphaned pending expression here so a stuck state always clears on the
    // next change rather than persisting.
    if (widget->expr != widget->pending_expr) {
        widget->expr = widget->pending_expr;
        set_idle_motion(widget);
    }

    if (widget->expr == id) {
        widget->pending_expr = id;
        return;
    }

    widget->pending_expr = id;

    if (id != EXPR_SLEEPY) {
        show_zzz(widget, false);
        if (zzz_timer) {
            lv_timer_pause(zzz_timer);
        }
    } else if (zzz_timer) {
        lv_timer_set_period(zzz_timer, ZZZ_DELAY_MS);
        lv_timer_reset(zzz_timer);
        lv_timer_resume(zzz_timer);
    }

    // Timed by where it's going, not where it's coming from - a snappy
    // expression should arrive snappily regardless of what preceded it.
    uint32_t close_ms = expressions[id].morph_ms ? (uint32_t)expressions[id].morph_ms / 2
                                                 : MORPH_CLOSE_MS;

    lv_anim_delete(widget, openness_anim_cb);
    animate(widget, openness_anim_cb, widget->openness, OPEN_SHUT, close_ms, 0,
            lv_anim_path_ease_in, morph_open);
}

static void blink_timer_cb(lv_timer_t *timer) {
    struct zmk_widget_eyes_status *widget = lv_timer_get_user_data(timer);
    const struct expression *e = &expressions[widget->expr];

    // The spiral doesn't blink: openness scales its radius, so a blink would
    // collapse and reinflate the whole shape, reading as the animation
    // restarting rather than as an eye closing.
    bool blinks = (e->shape != SHAPE_SPIRAL);

    // Never blink mid-morph. The blink drives the same object and exec_cb, so
    // starting one would replace the morph's animation and discard the
    // completion callback that adopts the new expression.
    bool morphing = (widget->expr != widget->pending_expr);

    if (blinks && !morphing && e->h > 24 && widget->openness == OPEN_FULL) {
        animate(widget, openness_anim_cb, OPEN_FULL, OPEN_SHUT, BLINK_CLOSE_MS, 0,
                lv_anim_path_ease_in, NULL);
        animate(widget, openness_anim_cb, OPEN_SHUT, OPEN_FULL, BLINK_OPEN_MS, BLINK_CLOSE_MS,
                lv_anim_path_ease_out, NULL);
    }

    lv_timer_set_period(timer, rnd_range(BLINK_MIN_MS, BLINK_MAX_MS));
}

static void glance_timer_cb(lv_timer_t *timer) {
    struct zmk_widget_eyes_status *widget = lv_timer_get_user_data(timer);
    const struct expression *e = &expressions[widget->expr];

    if (e->wander) {
        int16_t target = (int16_t)rnd_range(-GAZE_X_MAX, GAZE_X_MAX);
        widget->gaze_y = (int16_t)rnd_range(-GAZE_Y_MAX, GAZE_Y_MAX);
        animate(widget, gaze_anim_cb, widget->gaze_x, target, SACCADE_MS, 0, lv_anim_path_ease_out,
                NULL);
    }

    lv_timer_set_period(timer, rnd_range(GLANCE_MIN_MS, GLANCE_MAX_MS));
}

struct eyes_state {
    uint8_t layer;
    uint8_t wpm;
    bool idle;
};

static void update_wpm_level(uint8_t wpm) {
    switch (wpm_level) {
    case 2:
        if (wpm <= WPM_CONFUSED_OFF) {
            wpm_level = (wpm > WPM_SQUEEZE_OFF) ? 1 : 0;
        }
        break;
    case 1:
        if (wpm >= WPM_CONFUSED_ON) {
            wpm_level = 2;
        } else if (wpm <= WPM_SQUEEZE_OFF) {
            wpm_level = 0;
        }
        break;
    default:
        if (wpm >= WPM_CONFUSED_ON) {
            wpm_level = 2;
        } else if (wpm >= WPM_SQUEEZE_ON) {
            wpm_level = 1;
        }
        break;
    }
}

static enum expr_id resolve(struct eyes_state state) {
    // A layer with an expression of its own reports it, and that outranks
    // everything below. A layer without one - or one past the end of the table
    // - falls through to the activity and typing behaviour, so holding it
    // leaves the eyes doing whatever they were already doing.
    if (state.layer != 0 && state.layer < ARRAY_SIZE(layer_expr)) {
        enum expr_id id = layer_expr[state.layer];
        if (id != EXPR_NONE) {
            return id;
        }
    }

    if (state.idle) {
        return EXPR_SLEEPY;
    }

    update_wpm_level(state.wpm);

    // Squeezed reads as effort; past that it's just overwhelmed.
    switch (wpm_level) {
    case 2:
        return EXPR_CONFUSED;
    case 1:
        return EXPR_SQUEEZED;
    default:
        return EXPR_NEUTRAL;
    }
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

static void zzz_anim_opa(void *var, int32_t v) {
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)v, LV_PART_MAIN);
}

static void zzz_anim_y(void *var, int32_t v) { lv_obj_set_y((lv_obj_t *)var, v); }

static void init_zzz(struct zmk_widget_eyes_status *widget) {
    // Staggered so they read as a sequence rather than a pulse.
    static const int16_t zx[3] = {74, 86, 98};
    static const int16_t zy[3] = {-6, -22, -38};

    for (int i = 0; i < 3; i++) {
        widget->zzz[i] = lv_label_create(widget->obj);
        lv_label_set_text(widget->zzz[i], "z");
        lv_obj_set_style_text_font(widget->zzz[i], &Fredoka_SemiBold_20, LV_PART_MAIN);
        lv_obj_set_style_text_color(widget->zzz[i], lv_color_white(), LV_PART_MAIN);
        lv_obj_align(widget->zzz[i], LV_ALIGN_CENTER, zx[i], zy[i]);
        lv_obj_add_flag(widget->zzz[i], LV_OBJ_FLAG_HIDDEN);

        int16_t base = lv_obj_get_y(widget->zzz[i]);
        uint32_t delay = i * (ZZZ_CYCLE_MS / 3);

        lv_anim_t o;
        lv_anim_init(&o);
        lv_anim_set_var(&o, widget->zzz[i]);
        lv_anim_set_exec_cb(&o, zzz_anim_opa);
        lv_anim_set_values(&o, LV_OPA_TRANSP, LV_OPA_COVER);
        lv_anim_set_time(&o, ZZZ_CYCLE_MS / 2);
        lv_anim_set_playback_time(&o, ZZZ_CYCLE_MS / 2);
        lv_anim_set_delay(&o, delay);
        lv_anim_set_repeat_count(&o, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&o);

        // Snaps back to the start while opacity is zero, so the reset is unseen.
        lv_anim_t y;
        lv_anim_init(&y);
        lv_anim_set_var(&y, widget->zzz[i]);
        lv_anim_set_exec_cb(&y, zzz_anim_y);
        lv_anim_set_values(&y, base, base - ZZZ_RISE);
        lv_anim_set_time(&y, ZZZ_CYCLE_MS);
        lv_anim_set_delay(&y, delay);
        lv_anim_set_repeat_count(&y, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&y);
    }
}

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

        // Created before the line so it sits underneath it: the fill supplies
        // the interior, the stroke on top supplies smooth, rounded edges.
        widget->fill[i] = lv_canvas_create(widget->obj);
        lv_canvas_set_buffer(widget->fill[i], eye_fill_buf[i], EYE_FILL_W, EYE_FILL_H,
                             LV_COLOR_FORMAT_RGB565);
        lv_obj_add_flag(widget->fill[i], LV_OBJ_FLAG_HIDDEN);

        widget->line[i] = lv_line_create(widget->obj);
        lv_obj_remove_style_all(widget->line[i]);
        lv_obj_set_style_line_color(widget->line[i], lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_line_width(widget->line[i], LINE_W, LV_PART_MAIN);
        lv_obj_set_style_line_rounded(widget->line[i], true, LV_PART_MAIN);
        lv_obj_set_style_pad_all(widget->line[i], 0, LV_PART_MAIN);
        lv_obj_add_flag(widget->line[i], LV_OBJ_FLAG_HIDDEN);
    }

    init_zzz(widget);

    widget->expr = EXPR_NEUTRAL;
    widget->pending_expr = EXPR_NEUTRAL;
    widget->openness = OPEN_FULL;
    widget->strain = OPEN_FULL;
    widget->spin = 0;
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

    zzz_timer = lv_timer_create(zzz_timer_cb, ZZZ_DELAY_MS, widget);
    lv_timer_set_repeat_count(zzz_timer, 1);
    lv_timer_pause(zzz_timer);

    sys_slist_append(&widgets, &widget->node);

    widget_eyes_status_init();
    return 0;
}

lv_obj_t *zmk_widget_eyes_status_obj(struct zmk_widget_eyes_status *widget) { return widget->obj; }
