#ifndef LEVEL_GEO_H
#define LEVEL_GEO_H

/*
 * level_geo.h -- generates a static, flat-colored triangle mesh from
 * the tile map (src/map.h/map.c), mirroring what LevelBuilder.gd
 * does on the Godot client: one box per wall/platform tile, one
 * rotated box per ramp segment (same chain-detection algorithm,
 * ported from GDScript to C), plus a floor and ceiling plane.
 *
 */

#include "common.h"

typedef struct {
    f32 x, y, z;
    f32 r, g, b;
} GeoVertex;

typedef struct {
    GeoVertex *verts;
    int count;
    int capacity;
} VertexList;

/* Builds the full level's vertex list from the current map data.
 * Caller owns the returned list and must call vertex_list_free() on
 * it when done (e.g. after uploading to a GL buffer). */
VertexList build_level_geometry(void);
void       vertex_list_free(VertexList *vl);

/* Exposed so entity rendering (zombies, other players) can reuse the
 * same box-drawing code rather than duplicating it -- one flat-colored
 * axis-aligned box, given half-extents. */
void vlist_push(VertexList *vl, f32 x, f32 y, f32 z, f32 r, f32 g, f32 b);
void push_box(VertexList *vl, f32 cx, f32 cy, f32 cz, f32 hx, f32 hy, f32 hz,
             f32 r, f32 g, f32 b);

/* Returns the height (world Y) that standing on tile (mx, my) implies
 * -- 0 for plain floor, a fixed midpoint for ramp tiles, and
 * PLATFORM_HEIGHT for platform tiles. Used by the camera in main.c. */
f32 level_height_for_tile(int mx, int my);

#endif
