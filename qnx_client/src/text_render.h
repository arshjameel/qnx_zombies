/*
 * text_render.h -- real English text via the Public Pixel font atlas
 * (see font_atlas.h and tools/make_font_atlas.py).
 *
 * This is a SEPARATE rendering pipeline from hud_render.c's 7-segment
 * digits and main.c's flat-colored 2D quads (menu boxes, crosshair):
 * text needs a texture + UV coordinates, those don't. Keeping the
 * vertex format minimal for each (GeoVertex has no texcoord at all)
 * means text gets its own small vertex type and its own shader
 * program, rather than bloating every other vertex in the renderer
 * with fields only text needs. See main.c's TEXT_VERTEX_SHADER_SRC /
 * TEXT_FRAGMENT_SHADER_SRC and the third glUseProgram switch in the
 * render loop.
 */
#ifndef TEXT_RENDER_H
#define TEXT_RENDER_H

#include "common.h"

typedef struct {
    f32 x, y;       /* NDC, like hud_render.c's 2D elements -- always
                     * drawn with an identity/no-op transform, never
                     * part of the 3D world */
    f32 u, v;       /* texcoord into the font atlas */
    f32 r, g, b;
} TextVertex;

typedef struct {
    TextVertex *verts;
    int count;
    int capacity;
} TextVertexList;

void text_vlist_free(TextVertexList *vl);

/* Appends one string, left-to-right, starting at pixel position
 * (px, py) (top-left of the first glyph cell, same convention as
 * hud_push_number). char_w/char_h control the on-screen size of each
 * glyph -- independent of the atlas's native 16x16 texel cells, since
 * the shader samples the same atlas regardless of how large the quad
 * being drawn is. Unsupported characters (outside FONT_ATLAS_FIRST_CHAR
 * .. FONT_ATLAS_LAST_CHAR) are skipped but still advance the cursor,
 * so column alignment isn't thrown off by a stray unsupported byte. */
void text_push_string(TextVertexList *vl, const char *str, f32 px, f32 py,
                      f32 char_w, f32 char_h, int win_w, int win_h,
                      f32 r, f32 g, f32 b);

/* Pixel width text_push_string() would occupy for this string at this
 * char_w -- for centering/right-aligning, same role as
 * hud_number_width() plays for the numeric HUD. */
f32 text_string_width(const char *str, f32 char_w);

#endif /* TEXT_RENDER_H */
