#ifndef HUD_RENDER_H
#define HUD_RENDER_H

/*
 * hud_render.h -- minimal HUD drawing: numeric values as 7-segment
 * digits, plus a crosshair. GLES2 has no built-in text rendering, and
 * pulling in a font/texture-atlas library was out of scope for this
 * stage -- health/ammo/wave/zombie-count are all just integers, so a
 * 7-segment digit renderer covers every HUD need without needing any
 * font asset at all, consistent with the "no imported assets, build
 * everything from code" approach already used for the level geometry.
 *
 * The 7-segment on/off patterns per digit (0-9) are the standard,
 * universal hardware 7-segment-display encoding (segments a-g,
 * bit0=a...bit6=g) -- not QNX-specific, so no citation needed there.
 *
 * All coordinates in and out of this module are normalized device
 * coordinates (-1..1), matching what an identity MVP renders
 * directly -- callers pass in pixel positions/sizes and a window
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

/* Returns the pixel width hud_push_number() would occupy for this
 * value at this digit_w -- lets callers chain multiple numbers (e.g.
 * "current / max") left to right without hand-guessing spacing. Uses
 * the exact same digit-counting logic hud_push_number() does
 * internally, so the two can never disagree. */
f32 hud_number_width(int value, f32 digit_w);

/* Appends a small "+" crosshair centered on the screen. */
void hud_push_crosshair(VertexList *vl, int win_w, int win_h, f32 r, f32 g, f32 b);

/* Appends a flat-colored rectangle given pixel-space corners --
 * exposed so callers (e.g. a menu) can build custom 2D shapes reusing
 * the same tested pixel-to-NDC conversion this module already uses
 * internally for digits/crosshair. */
void hud_push_quad(VertexList *vl, f32 x0, f32 y0, f32 x1, f32 y1,
                   int win_w, int win_h, f32 r, f32 g, f32 b);

#endif /* HUD_RENDER_H */
