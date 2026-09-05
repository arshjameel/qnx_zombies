#include "hud_render.h"
#include <stdio.h>
#include <string.h>

static void px_to_ndc(f32 px, f32 py, int win_w, int win_h, f32 *nx, f32 *ny)
{
    *nx = (px / (f32)win_w) * 2.0f - 1.0f;
    *ny = 1.0f - (py / (f32)win_h) * 2.0f;  
}

void hud_push_quad(VertexList *vl, f32 x0, f32 y0, f32 x1, f32 y1,
                   int win_w, int win_h, f32 r, f32 g, f32 b)
{
    f32 nx0, ny0, nx1, ny1;
    px_to_ndc(x0, y0, win_w, win_h, &nx0, &ny0);
    px_to_ndc(x1, y1, win_w, win_h, &nx1, &ny1);
    
    vlist_push(vl, nx0, ny0, 0.0f, 0.0f, 0.0f, 1.0f, r, g, b);
    vlist_push(vl, nx1, ny0, 0.0f, 0.0f, 0.0f, 1.0f, r, g, b);
    vlist_push(vl, nx1, ny1, 0.0f, 0.0f, 0.0f, 1.0f, r, g, b);
    vlist_push(vl, nx0, ny0, 0.0f, 0.0f, 0.0f, 1.0f, r, g, b);
    vlist_push(vl, nx1, ny1, 0.0f, 0.0f, 0.0f, 1.0f, r, g, b);
    vlist_push(vl, nx0, ny1, 0.0f, 0.0f, 0.0f, 1.0f, r, g, b);
}

static const unsigned char DIGIT_SEGMENTS[10] = {
    0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F,
};

void hud_push_digit(VertexList *vl, int digit, f32 px, f32 py, f32 pw, f32 ph,
                    int win_w, int win_h, f32 r, f32 g, f32 b)
{
    unsigned char seg;
    f32 thick = pw * 0.22f;
    f32 mid_y = py + ph * 0.5f;

    if (digit < 0 || digit > 9) return;
    seg = DIGIT_SEGMENTS[digit];

    if (seg & 0x01) hud_push_quad(vl, px, py, px + pw, py + thick, win_w, win_h, r, g, b);                     /* a: top */
    if (seg & 0x02) hud_push_quad(vl, px + pw - thick, py, px + pw, mid_y, win_w, win_h, r, g, b);             /* b: top right */
    if (seg & 0x04) hud_push_quad(vl, px + pw - thick, mid_y, px + pw, py + ph, win_w, win_h, r, g, b);        /* c: bottom right */
    if (seg & 0x08) hud_push_quad(vl, px, py + ph - thick, px + pw, py + ph, win_w, win_h, r, g, b);           /* d: bottom */
    if (seg & 0x10) hud_push_quad(vl, px, mid_y, px + thick, py + ph, win_w, win_h, r, g, b);                  /* e: bottom left */
    if (seg & 0x20) hud_push_quad(vl, px, py, px + thick, mid_y, win_w, win_h, r, g, b);                       /* f: top left */
    if (seg & 0x40) hud_push_quad(vl, px, mid_y - thick * 0.5f, px + pw, mid_y + thick * 0.5f, win_w, win_h, r, g, b); /* g: middle */
}

void hud_push_number(VertexList *vl, int value, f32 px, f32 py, f32 digit_w, f32 digit_h,
                     int win_w, int win_h, f32 r, f32 g, f32 b)
{
    char buf[16];
    int  i;
    f32  x = px;
    f32  spacing = digit_w * 1.3f;

    if (value < 0) value = 0;   
    snprintf(buf, sizeof(buf), "%d", value);

    for (i = 0; buf[i] != '\0'; i++) {
        int d = buf[i] - '0';
        hud_push_digit(vl, d, x, py, digit_w, digit_h, win_w, win_h, r, g, b);
        x += spacing;
    }
}

f32 hud_number_width(int value, f32 digit_w)
{
    char buf[16];
    if (value < 0) value = 0;
    snprintf(buf, sizeof(buf), "%d", value);
    return (f32)strlen(buf) * digit_w * 1.3f;
}

void hud_push_crosshair(VertexList *vl, int win_w, int win_h, f32 r, f32 g, f32 b)
{
    f32 cx = (f32)win_w * 0.5f, cy = (f32)win_h * 0.5f;
    f32 half_len = 10.0f, half_thick = 1.5f;
    hud_push_quad(vl, cx - half_len, cy - half_thick, cx + half_len, cy + half_thick, win_w, win_h, r, g, b);
    hud_push_quad(vl, cx - half_thick, cy - half_len, cx + half_thick, cy + half_len, win_w, win_h, r, g, b);
}
