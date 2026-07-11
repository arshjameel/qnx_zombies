#ifndef LEVEL_GEO_H
#define LEVEL_GEO_H

/*
 * level_geo.h -- generates a static, flat-colored triangle mesh from
 * the tile map (src/map.h/map.c), mirroring what LevelBuilder.gd
 * does on the Godot client: one box per wall/platform tile, one
 * rotated box per ramp segment (same chain-detection algorithm,
 * ported from GDScript to C), plus a floor and ceiling plane.
 *
 * COUPLING WARNING: TILE_SIZE/WALL_HEIGHT/PLATFORM_HEIGHT/
 * PLATFORM_THICKNESS/RAMP_THICKNESS below are independent constants
 * from LevelBuilder.gd's -- there is no shared header between C and
 * GDScript for these (unlike the network protocol, which both C
 * clients get for free via net.h). If you want this client's level to
 * look the same size as the Godot client's, keep these numerically in
 * sync by hand.
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
 * axis-aligned box, given HALF-extents. */
void vlist_push(VertexList *vl, f32 x, f32 y, f32 z, f32 r, f32 g, f32 b);
void push_box(VertexList *vl, f32 cx, f32 cy, f32 cz, f32 hx, f32 hy, f32 hz,
             f32 r, f32 g, f32 b);

/* Returns the height (world Y) that standing on tile (mx, my) implies
 * -- 0 for plain floor, a fixed midpoint for ramp tiles, and
 * PLATFORM_HEIGHT for platform tiles. Deliberately the same
 * approximation src/server.c's zombie_height_tick() uses (raw current
 * tile type, not exact position-in-ramp-chain), for the same reason:
 * simple, consistent, and good enough to read as "climbing" without
 * needing to trace the precise slope. Used by the camera in main.c. */
f32 level_height_for_tile(int mx, int my);

/* Continuous (not tile-quantized) ramp height at exact position
 * (x, y), known to be on a ramp tile. See level_geo.c for why this
 * matters -- it's what actually makes climbing lag-free regardless
 * of movement speed, unlike the coarser level_height_for_tile(). */
f32 level_continuous_ramp_height(f32 x, f32 y);

/* Shared with main.c's camera logic (ramp/platform height queries) --
 * a real single source of truth, unlike EYE_HEIGHT-style constants
 * that have to be duplicated with a coupling comment across the
 * GDScript/C language boundary elsewhere in this project. Both this
 * file and main.c are C, compiled together, so there's no reason not
 * to share this directly. */
#define LEVEL_PLATFORM_HEIGHT 2.0f

#endif /* LEVEL_GEO_H */
