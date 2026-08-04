/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/display.h>
#include <zmk/event_manager.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/usb.h>
#include <zmk/ble.h>
#include <zmk/endpoints.h>

#include "output_status.h"
#include <fonts.h>

static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);

lv_point_t selection_line_points[] = {{0, 0}, {13, 0}}; // will be replaced with lv_point_precise_t

struct output_status_state
{
    struct zmk_endpoint_instance selected_endpoint;
    int active_profile_index;
    bool active_profile_connected;
    bool active_profile_bonded;
    bool usb_is_hid_ready;
};

static struct output_status_state get_state(const zmk_event_t *_eh)
{
    return (struct output_status_state){
        .selected_endpoint = zmk_endpoint_get_selected(),                  // 0 = USB , 1 = BLE
        .active_profile_index = zmk_ble_active_profile_index(),            // 0-3 BLE profiles
        .active_profile_connected = zmk_ble_active_profile_is_connected(), // 0 = not connected, 1 = connected
        .active_profile_bonded = !zmk_ble_active_profile_is_open(),        // 0 =  BLE not bonded, 1 = bonded
        .usb_is_hid_ready = zmk_usb_is_hid_ready()};                       // 0 = not ready, 1 = ready
}

static void set_status_symbol(struct zmk_widget_output_status *widget, struct output_status_state state)
{
    // Only the live transport is shown. The inactive one told you nothing you
    // couldn't infer, and cost a whole row.
    char transport_text[50] = {};
    char ble_text[12] = {};

    switch (state.selected_endpoint.transport)
    {
    case ZMK_TRANSPORT_USB:
        // Red when the dongle has power but the host isn't talking to it,
        // e.g. plugged into a charger rather than a computer.
        snprintf(transport_text, sizeof(transport_text), "#%s USB#",
                 state.usb_is_hid_ready ? "ffffff" : "ff0000");
        break;

    case ZMK_TRANSPORT_BLE:
    {
        const char *ble_color = "ffffff"; // profile free
        if (state.active_profile_connected)
        {
            ble_color = "00ff00";
        }
        else if (state.active_profile_bonded)
        {
            ble_color = "0000ff";
        }

        // Label and profile number go in one label. Two labels aligned to
        // opposite edges of a hand-sized box was what got them clipped.
        snprintf(transport_text, sizeof(transport_text), "#%s BLE %d#", ble_color,
                 state.active_profile_index + 1);
        break;
    }
    }

    lv_label_set_recolor(widget->transport_label, true);
    lv_obj_set_style_text_align(widget->transport_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(widget->transport_label, transport_text);
    lv_label_set_text(widget->ble_label, ble_text); // unused; kept empty
}

static void output_status_update_cb(struct output_status_state state)
{
    struct zmk_widget_output_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node)
    {
        set_status_symbol(widget, state);
    }
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_output_status, struct output_status_state,
                            output_status_update_cb, get_state)
ZMK_SUBSCRIPTION(widget_output_status, zmk_endpoint_changed);
ZMK_SUBSCRIPTION(widget_output_status, zmk_ble_active_profile_changed);
ZMK_SUBSCRIPTION(widget_output_status, zmk_usb_conn_state_changed);

// output_status.c
int zmk_widget_output_status_init(struct zmk_widget_output_status *widget, lv_obj_t *parent)
{
    widget->obj = lv_obj_create(parent);
    // Strip the default theme styling. A plain lv_obj carries padding and a
    // border, which inset the symbol from the left (reading as a gap) while
    // pushing the profile number out to the right, where the case lip clips
    // it. Without them the box is exactly the size it says it is.
    lv_obj_remove_style_all(widget->obj);
    // Sized to its content. Hand-picking a box is what clipped this before:
    // children are clipped to their parent, so every time the box was made
    // narrower to dodge the case lip, more of the symbol disappeared. The
    // 20px height was clipping it vertically too, against a ~25px line height.
    lv_obj_set_size(widget->obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);

    widget->transport_label = lv_label_create(widget->obj);
    lv_obj_set_style_text_font(widget->transport_label, &Fredoka_Regular_20, 0);
    lv_obj_align(widget->transport_label, LV_ALIGN_CENTER, 0, 0);

    widget->ble_label = lv_label_create(widget->obj);
    lv_obj_add_flag(widget->ble_label, LV_OBJ_FLAG_HIDDEN);

    sys_slist_append(&widgets, &widget->node);

    widget_output_status_init();
    return 0;
}

lv_obj_t *zmk_widget_output_status_obj(struct zmk_widget_output_status *widget)
{
    return widget->obj;
}
