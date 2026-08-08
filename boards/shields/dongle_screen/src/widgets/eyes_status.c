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

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>
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
// Tall enough to carry a band of dialogue above the eyes as well as the eyes
// themselves. Children are clipped to their parent's box, so this height is
// what dialogue has to live inside - it was 120, which left the sleep z's
// almost touching the ceiling.
//
// Grown symmetrically and the eyes are centred in it, so they do not move: the
// box simply reaches further up and down around them. The lower half goes
// unused, which costs nothing - the object is transparent and its children sit
// where they are told.
// 220 puts the top of the box exactly on the top of the screen: the widget is
// centred with a 10px lift on a 240px-tall panel, so this is as much room above
// the eyes as there is to have. Anything more would hang off the top and be
// clipped by the screen rather than gaining anything.
#define EYES_H 220

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

// How far the resting face narrows when it squints, as though focusing on
// something far off. Out of OPEN_FULL, and applied to the height directly now
// that the squint is an expression rather than an animation over neutral.
#define SQUINT_TO 110

// A saccade is a snap, not a drift: slow interpolation reads as the eyes
// sliding around the panel rather than looking at something.
#define SACCADE_MS 120
#define GLANCE_MIN_MS 1400
#define GLANCE_MAX_MS 4200
#define GAZE_X_MAX 14
#define GAZE_Y_MAX 6

// ZMK's WPM is a rolling estimate and bounces, so each threshold releases
// well below where it triggers.
#define WPM_SQUEEZE_ON 50
#define WPM_SQUEEZE_OFF 43
#define WPM_CONFUSED_ON 80
#define WPM_CONFUSED_OFF 72

// A "!" that pops up the moment typing starts, then takes itself away. Not a
// state like the expressions - a reaction, so it fires on the way past 5wpm
// rather than for as long as the figure sits above it, and releases low enough
// that a pause has to be real before it can fire again.
#define WPM_ALERT_ON 5
#define WPM_ALERT_OFF 2

// Dialogue is grouped by the event that prompts it, and one line is picked at
// random each time. Adding a variation is a line in the right list.
//
// Line breaks are written in, not wrapped automatically: each line gets its own
// label so its background can hug its own text, and the author is better placed
// than a wrap routine to decide where a remark should break.
//
// A line has 210px to work with before it runs past the widget and is clipped,
// which is a little over twenty lowercase characters. Nothing warns about
// overrunning it, so measure a long one rather than counting on the estimate.
//
// The font carries no punctuation beyond "!" - a comma or a full stop would
// render as nothing at all - so lines are written without it.

// Power-up, and every return from idle.
static const char *const DIALOGUE_WAKE[] = {
    "im awake now",
    "hello there",
    "why are we still here\njust to suffer",
};

// Typing has started.
static const char *const DIALOGUE_ALERT[] = {
    "!",
    "waow",
};

// Impatience, once typing has stopped for a while. Rolled each time typing
// stops rather than repeatedly while it stays stopped, so a long pause gets one
// chance at a remark and not a stream of them.
static const char *const DIALOGUE_NAG[] = {
    "are u gonna start\ntyping or what",
    "im hungry",
    "what now",
};
// A fresh delay is drawn for each pause rather than always landing on the same
// beat, so it does not read as a countdown you can predict.
#define NAG_MIN_MS 5000
#define NAG_MAX_MS 25000
#define NAG_CHANCE_PCT 60

// How long a finished remark sits before it starts to go. One number for every
// line, not one per line: because text is revealed as it is written, a long
// remark has already been read by the time it finishes, so it needs no more
// dwell at the end than a short one does.
#define DIALOGUE_HOLD_MS 1200
#define DIALOGUE_FADE_MS 400
// Drifts up as it goes, so a remark leaves rather than simply stopping being
// there. Small: the point is a suggestion of movement under the fade, not a
// journey. Small enough that even a full two lines, which already reach close
// to the top of the screen, do not climb off it on the way out.
#define DIALOGUE_RISE 10

// Revealed a few characters at a time, like a visual novel. Animations step at
// the refresh period, so this lands in a handful of frames rather than one
// character per frame - the point is that it arrives as text being written
// rather than a block appearing, without becoming something to wait through.
//
// The cap only guards against a remark far longer than anything written so far.
// It has to stay clear of the real lines, because a capped reveal races past
// reading speed - and the whole reason one hold suits every length is that the
// reading happens while the text is being written.
#define REVEAL_MS_PER_CHAR 30
#define REVEAL_MAX_MS 1200
// Breathing room inside the black plate, so glyphs are not flush to its edge.
#define DIALOGUE_PAD 3

// A line already being spoken is not interrupted by a lower-ranked one. The
// typing "!" is redundant on the heels of waking up, and cutting a sentence
// short to say it reads worse than not saying it at all. Equal ranks replace
// each other freely - chatter answering chatter is fine.
#define DIALOGUE_PRIO_CHATTER 0
#define DIALOGUE_PRIO_WAKE 1

// Dialogue: everything the buddy says beside its face - the sleep z's, the
// typing "!", and whatever comes later.
//
// Anchored to the top right of the widget rather than at an offset from its
// centre, so a longer line grows leftward into empty space instead of across
// the eyes, and sits in the band above them either way.
//
// It is otherwise independent of the face. Dialogue is not an expression and
// does not answer to one: an expression changing no longer clears it, and each
// piece is responsible for its own lifetime.
#define DIALOGUE_RIGHT 4

// Dialogue grows upward from a fixed baseline: the last line always lands here
// and earlier ones stack above it. A remark therefore expands into the empty
// band over the eyes rather than down across them, and its final line sits in
// the same place whether it is one line or two.
//
// 62 is the lowest value that leaves room for two lines plus the drift on the
// way out, given the box now starts at the top of the screen. It puts the
// bottom of a plate a few pixels into the top of the eyes, which the plate
// covers.
#define DIALOGUE_BOTTOM 62

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
// 2.5 turns rather than 2, with the stroke thinned to match: more turns over
// the same radius packs the coils closer, so holding the reference's roughly
// 1:1 stroke-to-gap means giving up some stroke width for them. 34 points
// because the extra half turn would otherwise stretch each segment.
#define SPIRAL_PTS 34
#define SPIRAL_TURNS 900 // degrees swept from centre to rim
// 1.5s whipped the free outer end round like a fan blade; 3.5s was sedate.
#define SPIN_MS 2400

// The sway has its own driver rather than being derived from `spin`, so the
// two are independent: each spiral keeps turning about its own centre at
// SPIN_MS while the pair together swings round a small circle at WOBBLE_MS.
// Deriving it from spin would also have snapped on every 359 -> 0 wrap.
//
// Fast and wide enough to read as a cartoon sway. It was 7s and 2px, which
// was a drift you had to look for.
#define WOBBLE_MS 1400
#define WOBBLE_PX 5

// ZMK only reports ACTIVE and IDLE here (ZMK_SLEEP is off), so the deeper
// "actually asleep" stage is timed locally.
#define ZZZ_DELAY_MS 20000
#define ZZZ_CYCLE_MS 1800
// The z's climb from the bottom slot, so the highest of them starts a line and
// a bit above the baseline. Kept short so that rise does not carry it past the
// top of the screen, where it would simply be clipped away.
#define ZZZ_RISE 12

enum eye_shape {
    SHAPE_BAR,
    SHAPE_CHEVRON_IN,
    SHAPE_ARC_DOWN,
    SHAPE_LIDDED,
    SHAPE_ANGRY,
    SHAPE_CROPPED,
    SHAPE_TWINKLE,
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

// How much of neutral the downward-looking variation cuts off the top. Subtle
// on purpose - just enough to flatten the top edge and read as a lowered lid.
// A flat edge is the point, and a rounded bar can't produce one, hence the clip.
#define CROP_TOP_PCT 14

// Rare transient variations on the resting face: a quirk timer swaps one in
// for a couple of seconds, then back. Scale factors are applied to width,
// height and radius alike so the silhouette stays neutral's, just resized.
#define QUIRK_MIN_MS 50000
#define QUIRK_MAX_MS 100000
#define QUIRK_HOLD_MS 2200
#define QUIRK_UP_PCT 116
#define QUIRK_SMALL_PCT 62

// The quirks draw hollow, which is what separates them from the resting face
// at a glance: same silhouette, no ink inside. 8px reads clearly as a ring at
// this size without closing up when a blink squashes it.
#define QUIRK_OUTLINE_W 8

// The small one breaks the pattern deliberately - a circle rather than
// neutral's rounded rectangle, so it reads as the eyes going round rather than
// merely shrinking. Sized off neutral's width so it stays proportional, which
// lands it between shock's 24 and neutral's 56.
#define QUIRK_SMALL_D (EYE_W * QUIRK_SMALL_PCT / 100)

// A sparkle punched clean through the eye: centred, filling nearly its whole
// height and width, sides bowed inward. Radius follows cos(2t) to the fourth,
// which puts sharp tips on the axes and pinches between them. The tips are
// taken as a proportion of the eye rather than fixed, so the sparkle keeps
// filling it if the eye is ever resized, and it comes out elongated because
// the eye is.
//
// It's a hole, not a black shape laid on top - at this size a stroke could
// never fill one, and a canvas over the eye would need alpha. Instead it's a
// second contour in the eye's own path: an even-odd fill crosses it twice and
// leaves it empty. The stroke is given only the outer contour, so the two
// bridging edges between the contours are never drawn.
// A diamond with slightly concave sides, not a four-pointed star. Both were
// tried as cos(2t) raised to a power, which is the wrong family: any version
// of it pinches to a waist between the tips, so it came out as a plus sign
// with fat arms or a thin cross with needles, never a diamond.
//
// A rhombus is 1/(|cos t| + |sin t|), which sits at 70.7% of the tip radius
// midway between two tips. Squaring the denominator bows the sides inward to
// 50%. Averaging the two lands at 60%: concave, but only slightly, which is
// what the shape wants.
#define SPARK_PTS 33
// Tip reach, as a proportion of the room inside the stroke. Measured against
// that rather than against the contour because the outline is centred on the
// contour and so covers half its width inward: tips taken all the way to the
// contour get painted back in, which flattened the left and right ones against
// the straight sides.
#define SPARK_FILL_PCT 100
#define SPARK_EDGE_W 2     // black outline, purely to anti-alias the hole's edge

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
    // Transient variations on neutral, chosen at random by the quirk timer.
    EXPR_NEUTRAL_UP,
    EXPR_NEUTRAL_DOWN,
    EXPR_NEUTRAL_SMALL,
    EXPR_NEUTRAL_TWINKLE,
    EXPR_NEUTRAL_SQUINT,
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
    bool no_blink;    // never blink in this expression
    // Bar shapes only: draw hollow with a border this wide instead of a solid
    // fill. LVGL insets a border, so an outlined eye occupies the same box as
    // a solid one and nothing shifts. 0 means solid.
    int16_t outline_w;
};

static const struct expression expressions[EXPR_COUNT] = {
    // The resting face. Both derived shapes below are cut out of this one, so
    // changing it changes them.
    [EXPR_NEUTRAL] = {SHAPE_BAR, EYE_W, EYE_H, 0, 0, EYE_R, true},
    // Doesn't blink either: these eyes are shut too, and the strain pulse
    // already moves them, so a blink on top competes with it.
    [EXPR_SQUEEZED] = {SHAPE_CHEVRON_IN, EYE_W, EYE_H, 0, 0, 0, false, 0, 0, 0, false, 0, true},
    [EXPR_SHOCK] = {SHAPE_BAR, 24, 24, 0, 0, 12, false},
    // Box height sets the bow: depth is h minus the stroke. Doesn't blink -
    // these eyes are already shut, so collapsing and reopening the arc reads
    // as a glitch rather than as a blink.
    [EXPR_SLEEPY] = {SHAPE_ARC_DOWN, EYE_W, 30, 0, 12, 0, false, 0, 0, 0, false, 0, true},
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
    [EXPR_CONFUSED] = {SHAPE_SPIRAL, 86, 86, 0, 0, 0, false, 8, 6},

    // Transient variations on the resting face, swapped in by the quirk timer.
    // Width, height and radius scale by the same factor, so these are neutral
    // resized rather than new shapes; the cropped one is neutral's own outline
    // with a flat slice off the top, so its curve is neutral's exactly.
    [EXPR_NEUTRAL_UP] = {SHAPE_BAR, EYE_W * QUIRK_UP_PCT / 100, EYE_H * QUIRK_UP_PCT / 100, 0, -7,
                         EYE_R * QUIRK_UP_PCT / 100, false, .outline_w = QUIRK_OUTLINE_W},
    // Hollow here means simply not filling the traced outline: the stroke that
    // used to smooth the fill's stepped edge becomes the whole shape, so the
    // sliced top stays exactly where it was.
    [EXPR_NEUTRAL_DOWN] = {SHAPE_CROPPED, EYE_W, EYE_H, 0, 7, 0, false, QUIRK_OUTLINE_W, 0, 0,
                           false},
    // LV_RADIUS_CIRCLE rather than half the width, so it stays perfectly round
    // at any size and turns into a lozenge rather than an odd rounded box when
    // a blink squashes it.
    [EXPR_NEUTRAL_SMALL] = {SHAPE_BAR, QUIRK_SMALL_D, QUIRK_SMALL_D, 0, 0, LV_RADIUS_CIRCLE, false,
                            .outline_w = QUIRK_OUTLINE_W},
    // Cuts in at 80ms like the layer expressions rather than easing over the
    // default 200. Animations step at the refresh period, so a 200ms morph is
    // sampled about twice: the plain quirks survive that because a growing
    // rounded rectangle reads fine half-drawn, but a half-formed sparkle just
    // looks like it arrived late.
    [EXPR_NEUTRAL_TWINKLE] = {SHAPE_TWINKLE, EYE_W, EYE_H, 0, 0, 0, false, 9, 0, 0, true, 80},
    // Neutral narrowed, as though focusing on something far off. Full width and
    // neutral's radius, which LVGL clamps to half the reduced height, so it
    // ends up a flattened pill rather than a squashed rounded rectangle.
    [EXPR_NEUTRAL_SQUINT] = {SHAPE_BAR, EYE_W, EYE_H * SQUINT_TO / OPEN_FULL, 0, 0, EYE_R, false,
                             .outline_w = QUIRK_OUTLINE_W},
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
        // Real entropy, not the uptime. Seeding from the clock looks fine for
        // the timers, which are first drawn on long enough for it to have
        // moved - but the greeting is chosen during widget init, when the
        // uptime is the same few hundred milliseconds on every boot. Same
        // seed, same sequence, same line every single time.
        rng_state = sys_rand32_get() | 1u;
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
// Neutral with a sparkle-shaped hole through it. The outer contour is
// neutral's outline; the sparkle follows as a second contour in the same
// array, which is what turns it into a hole under an even-odd fill.
//
// Point 20 repeats point 0. That closes the outer contour for the stroke, and
// makes both bridging edges - out to the sparkle and back - run between the
// same two points, so the bridge has no area and leaves no wedge in the fill.
static int set_twinkle_points(struct zmk_widget_eyes_status *widget, int eye, int32_t w,
                              int32_t box_h, int32_t inset) {
    const int32_t h = scaled(box_h, widget->openness);
    const int32_t top = (box_h - h) / 2 + inset;
    const int32_t eh = h - 2 * inset;
    const int32_t ew = w - 2 * inset;
    const int32_t cx = inset + ew / 2;
    const int32_t cy = top + eh / 2;
    const int32_t tip_x = (ew / 2 - inset) * SPARK_FILL_PCT / 100;
    const int32_t tip_y = (eh / 2 - inset) * SPARK_FILL_PCT / 100;
    lv_point_precise_t *p = widget->pts[eye];

    int n = rounded_rect(p, inset, top, ew, eh, EYE_R);
    p[n] = p[0];
    n++;

    for (int i = 0; i < SPARK_PTS; i++) {
        int32_t deg = (360 * (i % (SPARK_PTS - 1))) / (SPARK_PTS - 1);

        // Scaled down to 128ths before squaring: at full scale the square of
        // the denominator overflows a 32-bit int right at the diagonal.
        int32_t co = cos_of(deg);
        int32_t si = sin_of(deg);
        int32_t sn = ((co < 0 ? -co : co) + (si < 0 ? -si : si)) / 256;
        if (sn < 1) {
            sn = 1;
        }

        int32_t rhombus = (128 * TRIG_MAX) / sn;
        int32_t concave = (128 * 128 * TRIG_MAX) / (sn * sn);
        int32_t r = (rhombus + concave) / 2;

        // No openness scaling needed here: eh already carries it, so the
        // sparkle closes with the eye automatically.
        int32_t rx = (r * tip_x) / TRIG_MAX;
        int32_t ry = (r * tip_y) / TRIG_MAX;

        p[n].x = cx + (rx * cos_of(deg)) / TRIG_MAX;
        p[n].y = cy + (ry * sin_of(deg)) / TRIG_MAX;
        n++;
    }

    // Only the outer contour goes to the white stroke: the sparkle's edge is
    // internal, and stroking it white would fill the hole back in.
    lv_line_set_points(widget->line[eye], p, 21);

    // The sparkle's own points are contiguous from index 21, so its outline
    // can share them. Black, and thin - it exists to smooth the fill's stepped
    // edge, which has no anti-aliasing of its own, not to be seen.
    lv_line_set_points(widget->hole[eye], &p[21], SPARK_PTS);
    lv_obj_remove_flag(widget->hole[eye], LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(widget->hole[eye], w, box_h);

    return n;
}

// Neutral with a flat slice off the top. Same rounded rectangle and the same
// clip as angry, only the cut is level rather than slanted, so the bottom is
// neutral's own curve untouched.
static int set_cropped_points(struct zmk_widget_eyes_status *widget, int eye, int32_t w,
                              int32_t box_h, int32_t inset) {
    const int32_t h = scaled(box_h, widget->openness);
    const int32_t top = (box_h - h) / 2 + inset;
    const int32_t eh = h - 2 * inset;
    const int32_t x0 = inset;
    const int32_t ew = w - 2 * inset;
    const int32_t cut = top + (eh * CROP_TOP_PCT) / 100;

    lv_point_precise_t rr[EYE_MAX_PTS];
    lv_point_precise_t *p = widget->pts[eye];

    int n = rounded_rect(rr, x0, top, ew, eh, EYE_R);
    n = clip_below(p, rr, n, x0, ew, cut, cut);

    if (n < EYE_MAX_PTS) {
        p[n++] = p[0];
    }

    lv_line_set_points(widget->line[eye], p, n);
    return n;
}

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
    lv_draw_buf_t *db = lv_canvas_get_draw_buf(canvas);
    uint8_t *const base = db->data;
    const uint32_t stride = db->header.stride;

    // Black is all-zero bytes in RGB565, so the clear is one memset rather than
    // a per-pixel background fill.
    lv_memzero(base, stride * EYE_FILL_H);

    // Only scan the rows the polygon actually reaches. The canvas is sized for
    // the tallest expression, but this runs on every step of a morph, and for
    // most of one the eye is a few pixels tall inside eighty. Rows outside this
    // span can have no crossings, so skipping them changes nothing.
    int32_t y_top = EYE_FILL_H - 1;
    int32_t y_bot = 0;
    for (int i = 0; i < n; i++) {
        const int32_t y = (int32_t)p[i].y + oy;
        if (y < y_top) {
            y_top = y;
        }
        if (y > y_bot) {
            y_bot = y;
        }
    }
    y_top = MAX(y_top, 0);
    y_bot = MIN(y_bot, EYE_FILL_H - 1);

    for (int32_t y = y_top; y <= y_bot; y++) {
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

            // Direct spans rather than lv_canvas_set_px. That call re-reads the
            // draw buffer and switches on the colour format for every single
            // pixel, and this covers a few thousand per eye per frame - enough
            // to be felt as a hitch when the twinkle morphs in. White is 0xFFFF
            // in RGB565 whichever way round the bytes go, so LV_COLOR_16_SWAP
            // does not come into it.
            uint16_t *const row = (uint16_t *)(base + (uint32_t)y * stride);
            for (int32_t x = from; x <= to; x++) {
                row[x] = 0xFFFF;
            }
        }
    }

    // Writing into the buffer behind LVGL's back means saying so once, which is
    // cheaper than whatever set_px was doing per pixel regardless.
    lv_obj_invalidate(canvas);
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

        // Both spirals swing round the same circle in lockstep, so the whole
        // face sways rather than each eye wandering off on its own. Sharing
        // the phase is the point: independent drift reads as two loose
        // objects, linked drift reads as one dizzy head.
        if (shape == SHAPE_SPIRAL) {
            dx += (int16_t)((cos_of(widget->wob) * WOBBLE_PX) / TRIG_MAX);
            dy += (int16_t)((sin_of(widget->wob) * WOBBLE_PX) / TRIG_MAX);
        }

        if (shape == SHAPE_BAR) {
            int32_t h = scaled(box_h, widget->openness);
            if (h < 2) {
                h = 2;
            }

            lv_obj_add_flag(widget->fill[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(widget->line[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(widget->hole[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(widget->bar[i], LV_OBJ_FLAG_HIDDEN);

            lv_obj_set_size(widget->bar[i], e->w, h);
            lv_obj_set_style_radius(widget->bar[i], e->radius, LV_PART_MAIN);

            // Solid and hollow share one object, so both properties are written
            // every time rather than only on the branch that wants them - the
            // same trap the rotation fell into, where a value set on one
            // expression survived onto the next one to use this bar.
            lv_obj_set_style_bg_opa(widget->bar[i], e->outline_w ? LV_OPA_TRANSP : LV_OPA_COVER,
                                    LV_PART_MAIN);
            lv_obj_set_style_border_width(widget->bar[i], e->outline_w, LV_PART_MAIN);
            lv_obj_set_style_transform_pivot_x(widget->bar[i], e->w / 2, LV_PART_MAIN);
            lv_obj_set_style_transform_pivot_y(widget->bar[i], h / 2, LV_PART_MAIN);
            lv_obj_align(widget->bar[i], LV_ALIGN_CENTER, dx, dy);

            continue;
        }

        lv_obj_add_flag(widget->bar[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(widget->hole[i], LV_OBJ_FLAG_HIDDEN);
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
        case SHAPE_CROPPED:
            npts = set_cropped_points(widget, i, e->w, box_h, inset);
            break;
        case SHAPE_TWINKLE:
            npts = set_twinkle_points(widget, i, e->w, box_h, inset);
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

        if (!lv_obj_has_flag(widget->hole[i], LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_align(widget->hole[i], LV_ALIGN_CENTER, dx, dy);
            lv_obj_move_foreground(widget->hole[i]);
        }
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

// The sleep z's, each fading on its own stagger. Object-level opacity, so a
// plate fades with the text on it rather than leaving a rectangle behind.
static void fade_anim_opa(void *var, int32_t v) {
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)v, LV_PART_MAIN);
}

// Fades every line of the current remark together. Bound to the widget rather
// than to a label, so one animation covers however many lines are showing.
static void dialogue_fade_cb(void *var, int32_t v) {
    struct zmk_widget_eyes_status *widget = var;
    for (int i = 0; i < DIALOGUE_MAX_LINES; i++) {
        lv_obj_set_style_opa(widget->dialogue[i], (lv_opa_t)v, LV_PART_MAIN);
    }
}

static void dialogue_done(lv_anim_t *a) {
    struct zmk_widget_eyes_status *widget = a->var;
    for (int i = 0; i < DIALOGUE_MAX_LINES; i++) {
        lv_obj_add_flag(widget->dialogue[i], LV_OBJ_FLAG_HIDDEN);
    }
}

// Rank of the line currently being spoken. Only meaningful while its animation
// is still running, which is also how "still speaking" is decided.
static uint8_t dialogue_prio;

// The remark being revealed. Always a string literal from the lists above, so
// holding a pointer to it is safe for as long as the animation runs.
static const char *dialogue_text;

// Height of one line's plate, measured from the font at init. Kept here because
// the placement needs it to work out where each line sits.
static int16_t dialogue_line_h;

// The line currently taking characters. Placement keys off this rather than off
// the total, which is what makes a remark scroll: the active line always holds
// the bottom slot, and each earlier line has been pushed up one slot per line
// that followed it. So a second line does not appear below the first - the
// first rises out of the way and the second is typed where it was.
static int dialogue_active;

static int16_t dialogue_line_y(int i) {
    return (int16_t)(DIALOGUE_BOTTOM - (dialogue_active - i + 1) * dialogue_line_h);
}

// Lifts every line together on the way out. A separate callback from the fade
// so the two run as one movement without either replacing the other.
static void dialogue_rise_cb(void *var, int32_t rise) {
    struct zmk_widget_eyes_status *widget = var;
    for (int i = 0; i < DIALOGUE_MAX_LINES; i++) {
        lv_obj_set_y(widget->dialogue[i], dialogue_line_y(i) - (int16_t)rise);
    }
}

// Fills each line with as much of its text as has been revealed so far. Walks
// the whole remark every step rather than tracking a position, which costs
// nothing at these lengths and keeps the mapping from count to screen in one
// place.
static void dialogue_reveal_cb(void *var, int32_t shown) {
    struct zmk_widget_eyes_status *widget = var;

    // Found before anything is placed, because every line's position is
    // measured from it.
    dialogue_active = 0;
    {
        int left = shown;
        int i = 0;
        for (const char *p = dialogue_text; *p && i < DIALOGUE_MAX_LINES; i++) {
            const char *nl = strchr(p, '\n');
            int len = nl ? (int)(nl - p) : (int)strlen(p);
            if (left > 0) {
                dialogue_active = i;
            }
            left -= len;
            p = nl ? nl + 1 : p + len;
        }
    }

    int remaining = shown;
    int line = 0;

    for (const char *p = dialogue_text; *p && line < DIALOGUE_MAX_LINES; line++) {
        const char *nl = strchr(p, '\n');
        int len = nl ? (int)(nl - p) : (int)strlen(p);
        int show = remaining <= 0 ? 0 : MIN(remaining, len);

        lv_obj_t *o = widget->dialogue[line];
        // Re-placed every step: the slot a line occupies changes the moment the
        // next one starts.
        lv_obj_set_y(o, dialogue_line_y(line));
        if (show > 0) {
            lv_label_set_text_fmt(o, "%.*s", show, p);
            // Raised only on the way out of hidden. Doing it every step would
            // reorder the children on every frame for no gain.
            if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) {
                lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
                lv_obj_move_foreground(o);
            }
        } else {
            // Hidden rather than emptied: an empty label still draws its
            // padding, which would sit there as a small black tab waiting for
            // its first character.
            lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
        }

        remaining -= len;
        p = nl ? nl + 1 : p + len;
    }

    for (int i = line; i < DIALOGUE_MAX_LINES; i++) {
        lv_obj_add_flag(widget->dialogue[i], LV_OBJ_FLAG_HIDDEN);
    }
}

// Say one line: show it, hold at full opacity, then fade out and hide. One
// animation rather than a timer plus an animation, because the completion
// callback is the only thing that has to run, and speaking again simply
// replaces it rather than stacking.
//
// Nothing external takes it down. Dialogue is independent of the face, so a
// line runs its course whatever the eyes do in the meantime.
static void say(struct zmk_widget_eyes_status *widget, const char *text, uint8_t prio) {
    // An animation still running means a remark is still being spoken.
    if (lv_anim_get(widget, dialogue_fade_cb) != NULL && prio < dialogue_prio) {
        return;
    }
    dialogue_prio = prio;

    lv_anim_delete(widget, dialogue_fade_cb);
    lv_anim_delete(widget, dialogue_reveal_cb);
    lv_anim_delete(widget, dialogue_rise_cb);

    dialogue_text = text;

    // Newlines are breaks rather than characters and take no beat of their own.
    // Counted a line at a time so anything past the last slot is left out of
    // the total too, rather than the reveal spending time on text that will
    // never be shown.
    int total = 0;
    {
        int i = 0;
        for (const char *p = text; *p && i < DIALOGUE_MAX_LINES; i++) {
            const char *nl = strchr(p, '\n');
            int len = nl ? (int)(nl - p) : (int)strlen(p);
            total += len;
            p = nl ? nl + 1 : p + len;
        }
    }

    // Full opacity up front: the fade runs after a delay, and until it starts
    // its callback is not touching these.
    for (int i = 0; i < DIALOGUE_MAX_LINES; i++) {
        lv_obj_set_style_opa(widget->dialogue[i], LV_OPA_COVER, LV_PART_MAIN);
    }

    // Places everything back at the start, which also undoes any drift left by
    // a remark this one cut short - that would otherwise leave the next hanging
    // where the last had floated to.
    dialogue_reveal_cb(widget, 0);

    const uint32_t reveal_ms = MIN((uint32_t)total * REVEAL_MS_PER_CHAR, REVEAL_MAX_MS);

    lv_anim_t r;
    lv_anim_init(&r);
    lv_anim_set_var(&r, widget);
    lv_anim_set_exec_cb(&r, dialogue_reveal_cb);
    lv_anim_set_values(&r, 0, total);
    lv_anim_set_time(&r, reveal_ms);
    lv_anim_start(&r);

    // Distinct exec callbacks, so this coexists with the reveal rather than
    // replacing it - lv_anim keys on the variable and the callback together.
    // The hold begins once the last character has landed.
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, widget);
    lv_anim_set_exec_cb(&a, dialogue_fade_cb);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_delay(&a, reveal_ms + DIALOGUE_HOLD_MS);
    lv_anim_set_time(&a, DIALOGUE_FADE_MS);
    lv_anim_set_completed_cb(&a, dialogue_done);
    lv_anim_start(&a);

    // Rides alongside the fade on the same schedule. Eased out, so it lifts
    // away and settles rather than travelling at a constant rate to a stop.
    lv_anim_t u;
    lv_anim_init(&u);
    lv_anim_set_var(&u, widget);
    lv_anim_set_exec_cb(&u, dialogue_rise_cb);
    lv_anim_set_values(&u, 0, DIALOGUE_RISE);
    lv_anim_set_delay(&u, reveal_ms + DIALOGUE_HOLD_MS);
    lv_anim_set_time(&u, DIALOGUE_FADE_MS);
    lv_anim_set_path_cb(&u, lv_anim_path_ease_out);
    lv_anim_start(&u);
}

// Picks one of an event's lines at random. Adding a variation is a line in the
// list, nothing else.
#define SAY_ONE_OF(widget, lines, prio) say((widget), (lines)[rnd() % ARRAY_SIZE(lines)], (prio))

static void show_zzz(struct zmk_widget_eyes_status *widget, bool show) {
    for (int i = 0; i < 3; i++) {
        if (show) {
            lv_obj_remove_flag(widget->zzz[i], LV_OBJ_FLAG_HIDDEN);
            // Dialogue sits above the face wherever they meet.
            lv_obj_move_foreground(widget->zzz[i]);
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

    // Fires once per sleep, but pause it rather than letting a repeat count
    // expire: LVGL deletes a timer whose count runs out, and set_expression
    // needs this one to still exist to re-arm it on the next sleep.
    lv_timer_pause(timer);
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

    // The spiral is excluded by shape rather than by flag because openness
    // scales its radius: a blink would collapse and reinflate the whole thing,
    // reading as the animation restarting rather than as an eye closing.
    bool blinks = !e->no_blink && e->shape != SHAPE_SPIRAL;

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

// Which variation is currently standing in for the resting face, if any.
// resolve() only lets it through when the face would otherwise be neutral, so
// a layer or a burst of typing takes precedence and it simply lapses.
static enum expr_id quirk = EXPR_NONE;

static const enum expr_id quirks[] = {
    EXPR_NEUTRAL_UP,
    EXPR_NEUTRAL_DOWN,
    EXPR_NEUTRAL_SMALL,
    EXPR_NEUTRAL_TWINKLE,
    EXPR_NEUTRAL_SQUINT,
};

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
        // A quirk only stands in for the resting face; anything with something
        // to report has already returned above.
        return quirk != EXPR_NONE ? quirk : EXPR_NEUTRAL;
    }
}

// Edge-triggered, so the "!" fires once when typing starts rather than on every
// wpm report while it stays above the line. Releasing well below the trigger
// means a genuine pause has to happen before it can fire again.
static bool alert_armed;

static lv_timer_t *nag_timer;

static void nag_timer_cb(lv_timer_t *timer) {
    struct zmk_widget_eyes_status *widget = lv_timer_get_user_data(timer);

    // One shot per pause, by pausing rather than by a repeat count: LVGL
    // deletes a timer whose count reaches zero, and this one has to survive to
    // be re-armed the next time typing stops.
    lv_timer_pause(timer);

    // Asleep is a different situation with its own dialogue - the z's - and
    // being nagged awake would undercut them.
    if (widget->idle) {
        return;
    }

    if (rnd() % 100 >= NAG_CHANCE_PCT) {
        return;
    }

    SAY_ONE_OF(widget, DIALOGUE_NAG, DIALOGUE_PRIO_CHATTER);
}

static void update_alert(struct zmk_widget_eyes_status *widget, uint8_t wpm) {
    if (!alert_armed && wpm >= WPM_ALERT_ON) {
        alert_armed = true;

        // Typing resumed, so there is nothing left to be impatient about.
        if (nag_timer) {
            lv_timer_pause(nag_timer);
        }

        SAY_ONE_OF(widget, DIALOGUE_ALERT, DIALOGUE_PRIO_CHATTER);
    } else if (alert_armed && wpm <= WPM_ALERT_OFF) {
        alert_armed = false;

        // Typing stopped: arm the one remark this pause is allowed. Re-armed on
        // the falling edge only, so a pause that lasts an hour still gets a
        // single roll rather than one every time the delay elapses.
        if (nag_timer) {
            lv_timer_set_period(nag_timer, (uint32_t)rnd_range(NAG_MIN_MS, NAG_MAX_MS));
            lv_timer_reset(nag_timer);
            lv_timer_resume(nag_timer);
        }
    }
}

static void eyes_update_cb(struct eyes_state state) {
    struct zmk_widget_eyes_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
        // Edge, not level: said once on the way out of idle rather than for as
        // long as the keyboard stays awake. widget->idle starts false, so the
        // listener's own initial call cannot mistake boot for a wake - the
        // greeting at power-up is spoken explicitly instead.
        bool waking = widget->idle && !state.idle;
        widget->idle = state.idle;

        set_expression(widget, resolve(state));
        update_alert(widget, state.wpm);

        // Last, so it outranks the "!" that the same keypress is about to
        // trigger as the typing speed climbs past the threshold.
        if (waking) {
            SAY_ONE_OF(widget, DIALOGUE_WAKE, DIALOGUE_PRIO_WAKE);
        }
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

// One timer alternating between "pick one" and "put it back", rescheduling
// itself to the hold time or to the next long gap accordingly.
static void quirk_timer_cb(lv_timer_t *timer) {
    struct zmk_widget_eyes_status *widget = lv_timer_get_user_data(timer);

    if (quirk != EXPR_NONE) {
        quirk = EXPR_NONE;
        lv_timer_set_period(timer, rnd_range(QUIRK_MIN_MS, QUIRK_MAX_MS));
    } else if (widget->idle) {
        // Don't arm one while asleep. resolve() would suppress it anyway -
        // idle returns before the quirk is consulted - but arming it here
        // would mean waking up mid-quirk for no reason.
        lv_timer_set_period(timer, rnd_range(QUIRK_MIN_MS, QUIRK_MAX_MS));
    } else {
        quirk = quirks[rnd() % ARRAY_SIZE(quirks)];
        lv_timer_set_period(timer, QUIRK_HOLD_MS);
    }

    set_expression(widget, resolve(eyes_get_state(NULL)));
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_eyes_status, struct eyes_state, eyes_update_cb, eyes_get_state)
ZMK_SUBSCRIPTION(widget_eyes_status, zmk_layer_state_changed);
ZMK_SUBSCRIPTION(widget_eyes_status, zmk_wpm_state_changed);
ZMK_SUBSCRIPTION(widget_eyes_status, zmk_activity_state_changed);

static void zzz_anim_y(void *var, int32_t v) { lv_obj_set_y((lv_obj_t *)var, v); }

static void init_dialogue(struct zmk_widget_eyes_status *widget) {
    // Measured rather than guessed. A plate is the font's line box plus its
    // padding, and stacking by exactly that keeps consecutive lines touching
    // without either a gap between them or an overlap.
    const int16_t line_h = lv_font_get_line_height(&Fredoka_SemiBold_20) + 2 * DIALOGUE_PAD;
    dialogue_line_h = line_h;

    for (int i = 0; i < DIALOGUE_MAX_LINES; i++) {
        lv_obj_t *o = lv_label_create(widget->obj);
        widget->dialogue[i] = o;

        lv_obj_set_style_text_font(o, &Fredoka_SemiBold_20, LV_PART_MAIN);
        lv_obj_set_style_text_color(o, lv_color_white(), LV_PART_MAIN);

        // Opaque, and sized to this line's own text - a highlight behind the
        // words rather than a box drawn round the whole remark. Invisible
        // against the screen, which is black too; it earns its keep only where
        // a line crosses the eyes.
        lv_obj_set_style_bg_color(o, lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_pad_all(o, DIALOGUE_PAD, LV_PART_MAIN);
        lv_obj_set_width(o, LV_SIZE_CONTENT);

        // Right edges flush to a common margin, so lines stack against it and
        // each grows leftward into empty space. The vertical offset is set per
        // remark, since it depends on how many lines that remark has.
        lv_obj_align(o, LV_ALIGN_TOP_RIGHT, -DIALOGUE_RIGHT, dialogue_line_y(i));
        lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
}

static void init_zzz(struct zmk_widget_eyes_status *widget) {
    // Staggered so they read as a sequence rather than a pulse, offset from the
    // same top-right anchor the rest of the dialogue uses. The last one sits on
    // the anchor itself and the others trail down and to the left, so the group
    // keeps its diagonal while staying inside the band above the eyes.
    // Offsets up and to the right of the bottom slot, the same place a
    // one-line remark is typed. The first z sits on that baseline and the
    // others climb away from it.
    static const int16_t zx[3] = {-26, -13, 0};
    static const int16_t zy[3] = {0, 4, 8};

    for (int i = 0; i < 3; i++) {
        widget->zzz[i] = lv_label_create(widget->obj);
        lv_label_set_text(widget->zzz[i], "z");
        lv_obj_set_style_text_font(widget->zzz[i], &Fredoka_SemiBold_20, LV_PART_MAIN);
        lv_obj_set_style_text_color(widget->zzz[i], lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_bg_color(widget->zzz[i], lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(widget->zzz[i], LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_pad_all(widget->zzz[i], DIALOGUE_PAD, LV_PART_MAIN);
        lv_obj_align(widget->zzz[i], LV_ALIGN_TOP_RIGHT, -DIALOGUE_RIGHT + zx[i],
                     DIALOGUE_BOTTOM - dialogue_line_h - zy[i]);
        lv_obj_add_flag(widget->zzz[i], LV_OBJ_FLAG_HIDDEN);

        int16_t base = lv_obj_get_y(widget->zzz[i]);
        uint32_t delay = i * (ZZZ_CYCLE_MS / 3);

        lv_anim_t o;
        lv_anim_init(&o);
        lv_anim_set_var(&o, widget->zzz[i]);
        lv_anim_set_exec_cb(&o, fade_anim_opa);
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
        // Colour and opacity set once; only the width varies per expression,
        // and a width of zero draws nothing.
        lv_obj_set_style_border_color(widget->bar[i], lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_border_opa(widget->bar[i], LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_remove_flag(widget->bar[i], LV_OBJ_FLAG_SCROLLABLE);

        // Created before the line so it sits underneath it: the fill supplies
        // the interior, the stroke on top supplies smooth, rounded edges.
        widget->fill[i] = lv_canvas_create(widget->obj);
        lv_canvas_set_buffer(widget->fill[i], eye_fill_buf[i], EYE_FILL_W, EYE_FILL_H,
                             LV_COLOR_FORMAT_RGB565);
        lv_obj_add_flag(widget->fill[i], LV_OBJ_FLAG_HIDDEN);

        widget->hole[i] = lv_line_create(widget->obj);
        lv_obj_remove_style_all(widget->hole[i]);
        lv_obj_set_style_line_color(widget->hole[i], lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_line_width(widget->hole[i], SPARK_EDGE_W, LV_PART_MAIN);
        lv_obj_set_style_line_rounded(widget->hole[i], true, LV_PART_MAIN);
        lv_obj_set_style_pad_all(widget->hole[i], 0, LV_PART_MAIN);
        lv_obj_add_flag(widget->hole[i], LV_OBJ_FLAG_HIDDEN);

        widget->line[i] = lv_line_create(widget->obj);
        lv_obj_remove_style_all(widget->line[i]);
        lv_obj_set_style_line_color(widget->line[i], lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_line_width(widget->line[i], LINE_W, LV_PART_MAIN);
        lv_obj_set_style_line_rounded(widget->line[i], true, LV_PART_MAIN);
        lv_obj_set_style_pad_all(widget->line[i], 0, LV_PART_MAIN);
        lv_obj_add_flag(widget->line[i], LV_OBJ_FLAG_HIDDEN);
    }

    // Dialogue first: it measures the line height that the z's are placed
    // against.
    init_dialogue(widget);
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

    lv_timer_t *quirk_t =
        lv_timer_create(quirk_timer_cb, rnd_range(QUIRK_MIN_MS, QUIRK_MAX_MS), widget);
    lv_timer_set_repeat_count(quirk_t, -1);

    // Infinite like the rest, with the one-shot behaviour coming from the
    // callback pausing itself. A repeat count of 1 looks like the natural way
    // to say "once per sleep" and is a use-after-free: LVGL deletes the timer
    // when the count reaches zero, so the z's showed up once per boot and the
    // re-arm on every later sleep wrote to freed memory.
    zzz_timer = lv_timer_create(zzz_timer_cb, ZZZ_DELAY_MS, widget);
    lv_timer_set_repeat_count(zzz_timer, -1);
    lv_timer_pause(zzz_timer);

    // Same shape as the z's timer, and for the same reason: infinite repeat,
    // paused by its own callback, so it is still there to re-arm.
    // Period is a placeholder: it is paused immediately and gets a freshly
    // drawn delay each time typing stops.
    nag_timer = lv_timer_create(nag_timer_cb, NAG_MIN_MS, widget);
    lv_timer_set_repeat_count(nag_timer, -1);
    lv_timer_pause(nag_timer);

    sys_slist_append(&widgets, &widget->node);

    widget_eyes_status_init();

    // The greeting at power-up. After the listener's own initial update, which
    // would otherwise draw the face over a line already being spoken.
    SAY_ONE_OF(widget, DIALOGUE_WAKE, DIALOGUE_PRIO_WAKE);

    return 0;
}

lv_obj_t *zmk_widget_eyes_status_obj(struct zmk_widget_eyes_status *widget) { return widget->obj; }
