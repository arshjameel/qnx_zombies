#ifndef LEVEL_GEO_H
#define LEVEL_GEO_H

#include "common.h"

typedef struct {
    f32 x, y, z;
    f32 nx, ny, nz;   
    f32 r, g, b;
} GeoVertex;

typedef struct {
    GeoVertex *verts;
    int count;
    int capacity;
} VertexList;

VertexList build_level_geometry(void);
void       vertex_list_free(VertexList *vl);

void vlist_push(VertexList *vl, f32 x, f32 y, f32 z, f32 nx, f32 ny, f32 nz, f32 r, f32 g, f32 b);
void push_box(VertexList *vl, f32 cx, f32 cy, f32 cz, f32 hx, f32 hy, f32 hz,
             f32 r, f32 g, f32 b);

f32 level_height_for_tile(int mx, int my);

f32 level_continuous_ramp_height(f32 x, f32 y);

#define LEVEL_PLATFORM_HEIGHT 2.0f

#endif
