#ifndef TEXT_RENDER_H
#define TEXT_RENDER_H

#include "common.h"

typedef struct {
    f32 x, y;       
    f32 u, v;       
    f32 r, g, b;
} TextVertex;

typedef struct {
    TextVertex *verts;
    int count;
    int capacity;
} TextVertexList;

void text_vlist_free(TextVertexList *vl);

void text_push_string(TextVertexList *vl, const char *str, f32 px, f32 py,
                      f32 char_w, f32 char_h, int win_w, int win_h,
                      f32 r, f32 g, f32 b);

f32 text_string_width(const char *str, f32 char_w);

#endif 