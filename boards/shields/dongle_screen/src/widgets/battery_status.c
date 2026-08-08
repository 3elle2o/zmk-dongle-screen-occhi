/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/services/bas.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/battery.h>
#include <zmk/split/central.h>
#include <zmk/display.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/event_manager.h>
#include <zmk/usb.h>

#include "battery_status.h"
#include "../brightness.h"
#include <fonts.h>

#if IS_ENABLED(CONFIG_ZMK_DONGLE_DISPLAY_DONGLE_BATTERY)
#define SOURCE_OFFSET 1
#else
#define SOURCE_OFFSET 0
#endif

static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);

// The percentage sits beside the bar rather than on a row above it, so the
// whole indicator is one line tall instead of two.
// The number is the real indicator; the icon is a glanceable hint beside it,
// so it gives up width first when the row is tight.
//
// Drawn as an outlined body with a terminal on the end, not as a solid bar.
// A solid bar filling from the left cannot work on a black background: the
// empty part is invisible, so a half-full battery would read as a short stub
// rather than as a half-full battery.
#define BAT_BODY_W 28
#define BAT_NUB_W 2
// Kept even: the body is an even number of pixels tall, so an odd terminal
// height would sit a pixel off centre.
#define BAT_NUB_H 4
#define BAT_BAR_W (BAT_BODY_W + BAT_NUB_W)
#define BAT_BAR_H 10
// Wide enough for "100". Fredoka's digits are wider than Montserrat's, and at
// 28 a full charge would have been clipped by the label's own box - the same
// trap that caught the connection indicator.
#define BAT_LABEL_W 36
// Both cells sit left of centre so the connection indicator has the right-hand
// end of this row to itself.
//
// Pitch must exceed a cell's own width (BAT_LABEL_W + 4 + BAT_BAR_W = 68) or
// the cells overlap - which they silently did at 76 against an 80px cell,
// showing up as the two readouts touching. 92 leaves 24px between them.
#define BAT_PITCH 92

struct battery_state
{
    uint8_t source;
    uint8_t level;
    bool usb_present;
};

struct battery_object
{
    lv_obj_t *symbol;
    lv_obj_t *label;
} battery_objects[ZMK_SPLIT_CENTRAL_PERIPHERAL_COUNT + SOURCE_OFFSET];

static lv_color_t battery_image_buffer[ZMK_SPLIT_CENTRAL_PERIPHERAL_COUNT + SOURCE_OFFSET]
                                      [BAT_BAR_W * BAT_BAR_H];

// Peripheral reconnection tracking
// ZMK sends battery events with level < 1 when peripherals disconnect
static int8_t last_battery_levels[ZMK_SPLIT_CENTRAL_PERIPHERAL_COUNT + SOURCE_OFFSET];

// The last state each source reported, so any repaint can redraw all of them.
//
// ZMK_DISPLAY_WIDGET_LISTENER keeps a single state and a single work item, so
// two events arriving before the display thread services the queue collapse
// into one: the second overwrites the first, and re-submitting a work item
// that is already pending does nothing. One widget, two sources, one slot.
//
// The halves report on independent intervals and rarely collide, but they
// report together precisely when it matters - both powering down, or both
// joining at boot - and whichever lost the race was never drawn at all.
static struct battery_state source_states[ZMK_SPLIT_CENTRAL_PERIPHERAL_COUNT + SOURCE_OFFSET];

static void init_source_states(void)
{
    for (int i = 0; i < (ZMK_SPLIT_CENTRAL_PERIPHERAL_COUNT + SOURCE_OFFSET); i++)
    {
        // Level 0 draws as a red x, which is the honest state for a half that
        // has not reported yet. Starting here also means the row is complete
        // from boot rather than filling in a cell at a time.
        source_states[i] = (struct battery_state){.source = i, .level = 0};
    }
}

static void init_peripheral_tracking(void)
{
    for (int i = 0; i < (ZMK_SPLIT_CENTRAL_PERIPHERAL_COUNT + SOURCE_OFFSET); i++)
    {
        last_battery_levels[i] = -1; // -1 indicates never seen before
    }
}

static bool is_peripheral_reconnecting(uint8_t source, uint8_t new_level)
{
    if (source >= (ZMK_SPLIT_CENTRAL_PERIPHERAL_COUNT + SOURCE_OFFSET))
    {
        return false;
    }

    int8_t previous_level = last_battery_levels[source];

    // Reconnection detected if:
    // 1. Previous level was < 1 (disconnected/unknown) AND
    // 2. New level is >= 1 (valid battery level)
    bool reconnecting = (previous_level < 1) && (new_level >= 1);

    if (reconnecting)
    {
        LOG_INF("Peripheral %d reconnection: %d%% -> %d%% (was %s)",
                source, previous_level, new_level,
                previous_level == -1 ? "never seen" : "disconnected");
    }

    return reconnecting;
}

static void draw_battery(lv_obj_t *canvas, uint8_t level, bool usb_present)
{
    const lv_color_t bg = lv_color_black();
    lv_color_t fg;

    if (level < 1)
    {
        fg = lv_palette_main(LV_PALETTE_RED);
    }
    else if (level <= 10)
    {
        fg = lv_palette_main(LV_PALETTE_YELLOW);
    }
    else
    {
        fg = lv_color_white();
    }

    lv_canvas_fill_bg(canvas, bg, LV_OPA_COVER);

    // Body outline. Drawn pixel by pixel: lv_canvas_draw_rect doesn't exist in
    // LVGL v8+.
    for (int x = 0; x < BAT_BODY_W; x++)
    {
        lv_canvas_set_px(canvas, x, 0, fg, LV_OPA_COVER);
        lv_canvas_set_px(canvas, x, BAT_BAR_H - 1, fg, LV_OPA_COVER);
    }
    for (int y = 0; y < BAT_BAR_H; y++)
    {
        lv_canvas_set_px(canvas, 0, y, fg, LV_OPA_COVER);
        lv_canvas_set_px(canvas, BAT_BODY_W - 1, y, fg, LV_OPA_COVER);
    }

    // Clip the four corners so it doesn't read as a hard rectangle.
    lv_canvas_set_px(canvas, 0, 0, bg, LV_OPA_COVER);
    lv_canvas_set_px(canvas, BAT_BODY_W - 1, 0, bg, LV_OPA_COVER);
    lv_canvas_set_px(canvas, 0, BAT_BAR_H - 1, bg, LV_OPA_COVER);
    lv_canvas_set_px(canvas, BAT_BODY_W - 1, BAT_BAR_H - 1, bg, LV_OPA_COVER);

    // Terminal on the right-hand end.
    for (int x = BAT_BODY_W; x < BAT_BODY_W + BAT_NUB_W; x++)
    {
        for (int y = (BAT_BAR_H - BAT_NUB_H) / 2; y < (BAT_BAR_H + BAT_NUB_H) / 2; y++)
        {
            lv_canvas_set_px(canvas, x, y, fg, LV_OPA_COVER);
        }
    }

    // Charge, filling the interior from the left. Inset by the outline plus a
    // pixel of breathing room on every side.
    if (level > 0)
    {
        const int inner = BAT_BODY_W - 4;
        int filled = (level * inner) / 100;

        for (int x = 2; x < 2 + filled; x++)
        {
            for (int y = 2; y < BAT_BAR_H - 2; y++)
            {
                lv_canvas_set_px(canvas, x, y, fg, LV_OPA_COVER);
            }
        }
    }
}

static void set_battery_symbol(lv_obj_t *widget, struct battery_state state)
{
    if (state.source >= ZMK_SPLIT_CENTRAL_PERIPHERAL_COUNT + SOURCE_OFFSET)
    {
        return;
    }

    // Check for reconnection using the existing battery level mechanism
    bool reconnecting = is_peripheral_reconnecting(state.source, state.level);

    // Update our tracking
    last_battery_levels[state.source] = state.level;

    // Wake screen on reconnection
    if (reconnecting)
    {
#if CONFIG_DONGLE_SCREEN_IDLE_TIMEOUT_S > 0
        LOG_INF("Peripheral %d reconnected (battery: %d%%), requesting screen wake",
                state.source, state.level);
        brightness_wake_screen_on_reconnect();
#else
        LOG_INF("Peripheral %d reconnected (battery: %d%%)",
                state.source, state.level);
#endif
    }

    LOG_DBG("source: %d, level: %d, usb: %d", state.source, state.level, state.usb_present);
    lv_obj_t *symbol = battery_objects[state.source].symbol;
    lv_obj_t *label = battery_objects[state.source].label;

    draw_battery(symbol, state.level, state.usb_present);

    if (state.level > 0)
    {
        lv_obj_set_style_text_color(label, lv_color_white(), 0);
        lv_label_set_text_fmt(label, "%u", state.level);
    }
    else
    {
        lv_obj_set_style_text_color(label, lv_palette_main(LV_PALETTE_RED), 0);
        // Lowercase because the font carries no uppercase: the subset is
        // digits and a-z only. An "X" here would render as nothing, and this
        // is the glyph that says a half has dropped off.
        lv_label_set_text(label, "x");
    }

    if (state.level < 1)
    {
        lv_obj_set_style_text_color(label, lv_palette_main(LV_PALETTE_RED), 0);
        // Lowercase because the font carries no uppercase: the subset is
        // digits and a-z only. An "X" here would render as nothing, and this
        // is the glyph that says a half has dropped off.
        lv_label_set_text(label, "x");
    }
    else if (state.level <= 10)
    {
        lv_obj_set_style_text_color(label, lv_palette_main(LV_PALETTE_YELLOW), 0);
        lv_label_set_text_fmt(label, "%u", state.level);
    }
    else
    {
        lv_obj_set_style_text_color(label, lv_color_white(), 0);
        lv_label_set_text_fmt(label, "%u", state.level);
    }

    lv_obj_clear_flag(symbol, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(symbol);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(label);
}

void battery_status_update_cb(struct battery_state state)
{
    // The passed state is deliberately ignored: it is only ever the most recent
    // event, and the point here is to survive the ones that got coalesced away.
    // Redrawing every source costs two canvases and is idempotent.
    ARG_UNUSED(state);

    struct zmk_widget_dongle_battery_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node)
    {
        for (int i = 0; i < (ZMK_SPLIT_CENTRAL_PERIPHERAL_COUNT + SOURCE_OFFSET); i++)
        {
            set_battery_symbol(widget->obj, source_states[i]);
        }
    }
}

static struct battery_state peripheral_battery_status_get_state(const zmk_event_t *eh)
{
    const struct zmk_peripheral_battery_state_changed *ev = as_zmk_peripheral_battery_state_changed(eh);
    return (struct battery_state){
        .source = ev->source + SOURCE_OFFSET,
        .level = ev->state_of_charge,
    };
}

static struct battery_state central_battery_status_get_state(const zmk_event_t *eh)
{
    const struct zmk_battery_state_changed *ev = as_zmk_battery_state_changed(eh);
    return (struct battery_state){
        .source = 0,
        .level = (ev != NULL) ? ev->state_of_charge : zmk_battery_state_of_charge(),
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
        .usb_present = zmk_usb_is_powered(),
#endif /* IS_ENABLED(CONFIG_USB_DEVICE_STACK) */
    };
}

static void record_source(struct battery_state s)
{
    if (s.source < (ZMK_SPLIT_CENTRAL_PERIPHERAL_COUNT + SOURCE_OFFSET))
    {
        source_states[s.source] = s;
    }
}

static struct battery_state battery_status_get_state(const zmk_event_t *eh)
{
    // Recorded here rather than in the update callback because this runs for
    // every event, including the ones whose work submission is swallowed by an
    // already-pending one. That is exactly the case being fixed.
    if (as_zmk_peripheral_battery_state_changed(eh) != NULL)
    {
        struct battery_state s = peripheral_battery_status_get_state(eh);
        record_source(s);
        return s;
    }

    struct battery_state s = central_battery_status_get_state(eh);
#if IS_ENABLED(CONFIG_ZMK_DONGLE_DISPLAY_DONGLE_BATTERY)
    record_source(s);
#else
    // Without a dongle cell there is no source 0 of its own, and the listener's
    // one-off init call arrives here with a null event. Recording it would
    // paint the dongle's own charge into the first half's cell.
#endif
    return s;
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_dongle_battery_status, struct battery_state,
                            battery_status_update_cb, battery_status_get_state)

ZMK_SUBSCRIPTION(widget_dongle_battery_status, zmk_peripheral_battery_state_changed);

#if IS_ENABLED(CONFIG_ZMK_DONGLE_DISPLAY_DONGLE_BATTERY)
#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

ZMK_SUBSCRIPTION(widget_dongle_battery_status, zmk_battery_state_changed);
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
ZMK_SUBSCRIPTION(widget_dongle_battery_status, zmk_usb_conn_state_changed);
#endif /* IS_ENABLED(CONFIG_USB_DEVICE_STACK) */
#endif /* !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) */
#endif /* IS_ENABLED(CONFIG_ZMK_DONGLE_DISPLAY_DONGLE_BATTERY) */

int zmk_widget_dongle_battery_status_init(struct zmk_widget_dongle_battery_status *widget, lv_obj_t *parent)
{
    widget->obj = lv_obj_create(parent);

    lv_obj_set_size(widget->obj, 240, 20);

    for (int i = 0; i < ZMK_SPLIT_CENTRAL_PERIPHERAL_COUNT + SOURCE_OFFSET; i++)
    {
        lv_obj_t *image_canvas = lv_canvas_create(widget->obj);
        lv_obj_t *battery_label = lv_label_create(widget->obj);

        lv_canvas_set_buffer(image_canvas, battery_image_buffer[i], BAT_BAR_W, BAT_BAR_H,
                             LV_COLOR_FORMAT_RGB565);

        // Number then bar, both on the same line, one cell per half.
        lv_obj_set_style_text_font(battery_label, &Fredoka_SemiBold_20, 0);
        lv_obj_set_width(battery_label, BAT_LABEL_W);
        lv_obj_set_style_text_align(battery_label, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(battery_label, LV_ALIGN_LEFT_MID, i * BAT_PITCH, 0);
        lv_obj_align(image_canvas, LV_ALIGN_LEFT_MID, BAT_LABEL_W + 4 + (i * BAT_PITCH), 0);

        lv_obj_add_flag(image_canvas, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(battery_label, LV_OBJ_FLAG_HIDDEN);

        battery_objects[i] = (struct battery_object){
            .symbol = image_canvas,
            .label = battery_label,
        };
    }

    sys_slist_append(&widgets, &widget->node);

    // Initialize peripheral tracking
    init_peripheral_tracking();
    init_source_states();

    widget_dongle_battery_status_init();

    return 0;
}

lv_obj_t *zmk_widget_dongle_battery_status_obj(struct zmk_widget_dongle_battery_status *widget)
{
    return widget->obj;
}
