/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <lvgl.h>
#include <zephyr/kernel.h>

// Chevrons need 3 points, the star 11; the confused spiral needs the rest.
#define EYE_MAX_PTS 30

// Each eye owns both a bar and a line object. Expressions swap which one is
// visible rather than creating and deleting objects on the display thread.
struct zmk_widget_eyes_status {
    sys_snode_t node;
    lv_obj_t *obj;
    lv_obj_t *bar[2];
    lv_obj_t *line[2];
    lv_point_precise_t pts[2][EYE_MAX_PTS];
    lv_obj_t *zzz[3]; // drift up and fade once idle has gone on a while

    uint8_t expr;
    uint8_t pending_expr; // expression to adopt at the bottom of a transition

    int16_t openness; // 0-256, scales vertical extent; drives blinks and morphs
    int16_t strain;   // 0-256, how hard a squeeze is currently pushing
    int16_t spin;     // 0-359, rotation of the confused spiral
    int16_t gaze_x;
    int16_t gaze_y;
    bool idle;
};

int zmk_widget_eyes_status_init(struct zmk_widget_eyes_status *widget, lv_obj_t *parent);
lv_obj_t *zmk_widget_eyes_status_obj(struct zmk_widget_eyes_status *widget);
