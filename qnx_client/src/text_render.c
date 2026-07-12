/*
 * text_render.c -- see text_render.h for the overview.
 */

#include "text_render.h"
#include "font_atlas.h"

#include <stdlib.h>
#include <string.h>

/* Same pixel-to-NDC convention as hud_render.c's px_to_ndc(), kept as
 * its own small copy rather than a cross-module dependency for
 * something this tiny -- both modules stay independent. */
static void px_to_ndc(f32 px, f32 py, int win_w, int win_h, f32 *nx, f32 *ny)
{
    *nx = (px / (f32)win_w) * 2.0f - 1.0f;
    *ny = 1.0f - (py / (f32)win_h) * 2.0f;
}

static void text_vlist_push(TextVertexList *vl, f32 x, f32 y, f32 u, f32 v,
                            f32 r, f32 g, f32 b)
{
    if (vl->count >= vl->capacity) {
        vl->capacity = vl->capacity ? vl->capacity * 2 : 256;
        vl->verts = (TextVertex *)realloc(vl->verts, sizeof(TextVertex) * (size_t)vl->capacity);
    }
    vl->verts[vl->count].x = x; vl->verts[vl->count].y = y;
    vl->verts[vl->count].u = u; vl->verts[vl->count].v = v;
    vl->verts[vl->count].r = r; vl->verts[vl->count].g = g; vl->verts[vl->count].b = b;
    vl->count++;
}

void text_vlist_free(TextVertexList *vl)
{
    free(vl->verts);
    vl->verts = NULL;
    vl->count = vl->capacity = 0;
}

/* One glyph quad: pixel-space (px0,py0)-(px1,py1) mapped to NDC, UVs
 * looked up from the atlas grid for this ASCII code. */
static void push_glyph(TextVertexList *vl, int code, f32 px0, f32 py0, f32 px1, f32 py1,
                       int win_w, int win_h, f32 r, f32 g, f32 b)
{
    int index = code - FONT_ATLAS_FIRST_CHAR;
    int col = index % FONT_ATLAS_COLS;
    int row = index / FONT_ATLAS_COLS;
    f32 u0 = (f32)(col * FONT_ATLAS_CELL) / (f32)FONT_ATLAS_W;
    f32 v0 = (f32)(row * FONT_ATLAS_CELL) / (f32)FONT_ATLAS_H_PX;
    f32 u1 = (f32)((col + 1) * FONT_ATLAS_CELL) / (f32)FONT_ATLAS_W;
    f32 v1 = (f32)((row + 1) * FONT_ATLAS_CELL) / (f32)FONT_ATLAS_H_PX;
    f32 nx0, ny0, nx1, ny1;

    px_to_ndc(px0, py0, win_w, win_h, &nx0, &ny0);
    px_to_ndc(px1, py1, win_w, win_h, &nx1, &ny1);

    text_vlist_push(vl, nx0, ny0, u0, v0, r, g, b);
    text_vlist_push(vl, nx1, ny0, u1, v0, r, g, b);
    text_vlist_push(vl, nx1, ny1, u1, v1, r, g, b);
    text_vlist_push(vl, nx0, ny0, u0, v0, r, g, b);
    text_vlist_push(vl, nx1, ny1, u1, v1, r, g, b);
    text_vlist_push(vl, nx0, ny1, u0, v1, r, g, b);
}

void text_push_string(TextVertexList *vl, const char *str, f32 px, f32 py,
                      f32 char_w, f32 char_h, int win_w, int win_h,
                      f32 r, f32 g, f32 b)
{
    f32 x = px;
    const char *c;
    for (c = str; *c != '\0'; c++) {
        int code = (unsigned char)*c;
        if (code >= FONT_ATLAS_FIRST_CHAR && code <= FONT_ATLAS_LAST_CHAR) {
            push_glyph(vl, code, x, py, x + char_w, py + char_h, win_w, win_h, r, g, b);
        }
        /* still advance on an unsupported byte -- see header comment */
        x += char_w;
    }
}

f32 text_string_width(const char *str, f32 char_w)
{
    return (f32)strlen(str) * char_w;
}
