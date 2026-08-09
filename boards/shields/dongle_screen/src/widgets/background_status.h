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

// The popping-vein cross that hangs in the air around an angry character.
// Twelve points - four tapered arms - plus a repeat of the first to close it.
#define BG_ANGER_MARKS 6
#define BG_ANGER_PTS 13

struct zmk_widget_background {
    sys_snode_t node;
    lv_obj_t *obj;

    lv_obj_t *sparkle[BG_SPARKLES];
    lv_point_precise_t pts[BG_SPARKLES][BG_SPARKLE_PTS];

    // A wash behind the strokes, so they read as falling out of something
    // rather than floating on black.
    lv_obj_t *stress_grad;
    lv_obj_t *stress[BG_STRESS_LINES];

    lv_obj_t *anger[BG_ANGER_MARKS];
    lv_point_precise_t anger_pts[BG_ANGER_MARKS][BG_ANGER_PTS];
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

// Anger marks on or off, called by whatever is drawing the face. Deliberately
// not derived from the layer here: the eyes own the mapping from layer to
// expression, and a second copy of it in this file would be a copy to keep in
// step. Takes no widget, so a caller needs no handle on this one.
void zmk_widget_background_set_anger(bool on);
