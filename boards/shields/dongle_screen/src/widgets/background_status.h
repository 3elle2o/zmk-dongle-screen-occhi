/*
 * SPDX-License-Identifier: MIT
 *
 * The layer behind everything else. Nothing here is information - it exists to
 * give the face something to sit in front of, so it is deliberately dimmer than
 * the eyes and the dialogue and must never compete with them for attention.
 */

#pragma once

#include <lvgl.h>
#include <zephyr/kernel.h>

// Enough to read as a scattering rather than a handful, few enough that they
// stay incidental.
#define BG_SPARKLES 10

// Four tips and four waists, plus a repeat of the first point to close the
// outline.
#define BG_SPARKLE_PTS 9

struct zmk_widget_background {
    sys_snode_t node;
    lv_obj_t *obj;
    lv_obj_t *sparkle[BG_SPARKLES];
    lv_point_precise_t pts[BG_SPARKLES][BG_SPARKLE_PTS];
};

int zmk_widget_background_init(struct zmk_widget_background *widget, lv_obj_t *parent);
lv_obj_t *zmk_widget_background_obj(struct zmk_widget_background *widget);

// Scatters the sparkles, fades them in and out once on a stagger, and puts them
// away. Safe to call again while one is running - it simply restarts.
void zmk_widget_background_sparkle_burst(struct zmk_widget_background *widget);
