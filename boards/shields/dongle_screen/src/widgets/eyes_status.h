/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <lvgl.h>
#include <zephyr/kernel.h>

// Each eye owns both a bar and a line object. Expressions swap which one is
// visible rather than creating and deleting objects on the display thread.
struct zmk_widget_eyes_status {
    sys_snode_t node;
    lv_obj_t *obj;
    lv_obj_t *bar[2];
    lv_obj_t *line[2];
    lv_point_precise_t pts[2][3];

    uint8_t expr;     // index into the expression table
    int16_t cur_h;    // animated by the blink
    int16_t gaze_x;   // saccade offset, applied to both eyes together
    int16_t gaze_y;
    bool idle;
};

int zmk_widget_eyes_status_init(struct zmk_widget_eyes_status *widget, lv_obj_t *parent);
lv_obj_t *zmk_widget_eyes_status_obj(struct zmk_widget_eyes_status *widget);
