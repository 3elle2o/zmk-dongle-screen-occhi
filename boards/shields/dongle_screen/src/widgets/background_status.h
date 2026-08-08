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

// Vertical strokes hanging from the top of the panel, which fill in as typing
// speed climbs. Enough to read as a wall of them at full intensity without
// turning the top of the screen solid.
#define BG_STRESS_LINES 16

struct zmk_widget_background {
    sys_snode_t node;
    lv_obj_t *obj;

    lv_obj_t *sparkle[BG_SPARKLES];
    lv_point_precise_t pts[BG_SPARKLES][BG_SPARKLE_PTS];

    lv_obj_t *stress[BG_STRESS_LINES];
    lv_point_precise_t stress_pts[BG_STRESS_LINES][2];
    // Per-line share of the full opacity, so they do not all come up together
    // like a comb.
    uint8_t stress_weight[BG_STRESS_LINES];
};

int zmk_widget_background_init(struct zmk_widget_background *widget, lv_obj_t *parent);
lv_obj_t *zmk_widget_background_obj(struct zmk_widget_background *widget);

// Scatters the sparkles, fades them in and out once on a stagger, and puts them
// away. Safe to call again while one is running - it simply restarts.
void zmk_widget_background_sparkle_burst(struct zmk_widget_background *widget);
