#ifndef HUD_RENDER_H
#define HUD_RENDER_H

/*
 * hud_render.h -- minimal HUD drawing: numeric values as 7-segment
 * digits, plus a crosshair.
 *
 * All coordinates in and out of this module are normalized device
 * coordinates (-1..1), matching what an identity MVP renders
 * directly: callers pass in pixel positions/sizes and a window
 * size, and this module does the pixel-to-NDC conversion internally.
 */

#include "level_geo.h"   /* VertexList, vlist_push */

/* Appends the 7-segment quads for a single digit (0-9) into vl.
 * (px, py) is the digit's top-left corner in pixels; win_w/win_h are
 * the window's actual size (for the pixel->NDC conversion). */
void hud_push_digit(VertexList *vl, int digit, f32 px, f32 py, f32 pw, f32 ph,
                    int win_w, int win_h, f32 r, f32 g, f32 b);

/* Appends every digit of a non-negative integer, left to right,
 * starting at (px, py). */
void hud_push_number(VertexList *vl, int value, f32 px, f32 py, f32 digit_w, f32 digit_h,
                     int win_w, int win_h, f32 r, f32 g, f32 b);

/* Appends a small "+" crosshair centered on the screen. */
void hud_push_crosshair(VertexList *vl, int win_w, int win_h, f32 r, f32 g, f32 b);

#endif
