#ifndef TEXT_RENDER_H
#define TEXT_RENDER_H

/*
 * text_render.h -- 5x7 bitmap font for general alphanumeric text
 * (menu title, options, credits), separate from hud_render.c's
 * 7-segment digits (which only ever need to show numbers). Designed
 * from scratch, not copied from any existing font file -- verified by
 * rendering every glyph as ASCII art and visually checking each one
 * before it went into the C table (see the project's dev notes/git
 * history for that verification pass).
 *
 * Same approach as hud_render.c: works in pixel space, converts to
 * NDC internally, so callers just pass pixel positions/sizes and the
 * window size.
 */

#include "level_geo.h"   /* VertexList, vlist_push */

/* Appends one character's "on" pixels as small quads, top-left corner
 * at (px, py) in screen pixels, each "pixel" scaled to (pixel_w,
 * pixel_h). Lowercase is folded to uppercase; anything outside
 * A-Z/0-9/space renders blank rather than garbage. */
void text_push_char(VertexList *vl, char c, f32 px, f32 py, f32 pixel_w, f32 pixel_h,
                    int win_w, int win_h, f32 r, f32 g, f32 b);

/* Appends a full string, left to right, starting at (px, py). Returns
 * the total width in pixels the string occupies. */
f32 text_push_string(VertexList *vl, const char *str, f32 px, f32 py,
                     f32 pixel_w, f32 pixel_h, int win_w, int win_h,
                     f32 r, f32 g, f32 b);

/* Returns the width in pixels a string WOULD occupy, without drawing
 * anything -- used to center text on the X axis: compute this, then
 * pass (win_w - width) / 2 as px to text_push_string. */
f32 text_measure_width(const char *str, f32 pixel_w);

#endif /* TEXT_RENDER_H */
