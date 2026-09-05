#ifndef HUD_RENDER_H
#define HUD_RENDER_H

#include "level_geo.h"  

void hud_push_digit(VertexList *vl, int digit, f32 px, f32 py, f32 pw, f32 ph,
                    int win_w, int win_h, f32 r, f32 g, f32 b);

void hud_push_number(VertexList *vl, int value, f32 px, f32 py, f32 digit_w, f32 digit_h,
                     int win_w, int win_h, f32 r, f32 g, f32 b);

f32 hud_number_width(int value, f32 digit_w);

/* "+" crosshair on screen. */
void hud_push_crosshair(VertexList *vl, int win_w, int win_h, f32 r, f32 g, f32 b);

void hud_push_quad(VertexList *vl, f32 x0, f32 y0, f32 x1, f32 y1,
                   int win_w, int win_h, f32 r, f32 g, f32 b);

#endif
