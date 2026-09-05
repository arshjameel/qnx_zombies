#include "level_geo.h"
#include "mat4.h"
#include "map.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define TILE_SIZE          1.0f
#define WALL_HEIGHT        4.0f
#define PLATFORM_HEIGHT    LEVEL_PLATFORM_HEIGHT
#define PLATFORM_THICKNESS 0.25f
#define RAMP_THICKNESS     0.3f

void vlist_push(VertexList *vl, f32 x, f32 y, f32 z, f32 nx, f32 ny, f32 nz, f32 r, f32 g, f32 b)
{
    if (vl->count >= vl->capacity) {
        vl->capacity = vl->capacity ? vl->capacity * 2 : 1024;
        vl->verts = (GeoVertex *)realloc(vl->verts, sizeof(GeoVertex) * (size_t)vl->capacity);
    }
    vl->verts[vl->count].x = x; vl->verts[vl->count].y = y; vl->verts[vl->count].z = z;
    vl->verts[vl->count].nx = nx; vl->verts[vl->count].ny = ny; vl->verts[vl->count].nz = nz;
    vl->verts[vl->count].r = r; vl->verts[vl->count].g = g; vl->verts[vl->count].b = b;
    vl->count++;
}

void vertex_list_free(VertexList *vl)
{
    free(vl->verts);
    vl->verts = NULL;
    vl->count = vl->capacity = 0;
}

static const int FACES[6][4] = {
    {0,1,2,3}, {5,4,7,6}, {4,0,3,7}, {1,5,6,2}, {3,2,6,7}, {4,5,1,0},
};

static const f32 FACE_NORMALS[6][3] = {
    { 0,  0, -1}, { 0,  0,  1}, {-1,  0,  0}, { 1,  0,  0}, { 0,  1,  0}, { 0, -1,  0},
};

static void emit_box_faces(VertexList *vl, const f32 v[8][3], const f32 normals[6][3],
                           f32 r, f32 g, f32 b)
{
    int f;
    for (f = 0; f < 6; f++) {
        int a = FACES[f][0], bI = FACES[f][1], c = FACES[f][2], d = FACES[f][3];
        f32 nx = normals[f][0], ny = normals[f][1], nz = normals[f][2];
        vlist_push(vl, v[a][0],  v[a][1],  v[a][2],  nx, ny, nz, r, g, b);
        vlist_push(vl, v[bI][0], v[bI][1], v[bI][2], nx, ny, nz, r, g, b);
        vlist_push(vl, v[c][0],  v[c][1],  v[c][2],  nx, ny, nz, r, g, b);
        vlist_push(vl, v[a][0],  v[a][1],  v[a][2],  nx, ny, nz, r, g, b);
        vlist_push(vl, v[c][0],  v[c][1],  v[c][2],  nx, ny, nz, r, g, b);
        vlist_push(vl, v[d][0],  v[d][1],  v[d][2],  nx, ny, nz, r, g, b);
    }
}

void push_box(VertexList *vl, f32 cx, f32 cy, f32 cz, f32 hx, f32 hy, f32 hz,
             f32 r, f32 g, f32 b)
{
    f32 x0 = cx-hx, x1 = cx+hx;
    f32 y0 = cy-hy, y1 = cy+hy;
    f32 z0 = cz-hz, z1 = cz+hz;
    f32 v[8][3] = {
        {x0,y0,z0},{x1,y0,z0},{x1,y1,z0},{x0,y1,z0},
        {x0,y0,z1},{x1,y0,z1},{x1,y1,z1},{x0,y1,z1},
    };
    emit_box_faces(vl, v, FACE_NORMALS, r, g, b);
}

static void push_box_transformed(VertexList *vl, Mat4 world, f32 hx, f32 hy, f32 hz,
                                 f32 r, f32 g, f32 b)
{
    f32 lv[8][3] = {
        {-hx,-hy,-hz},{hx,-hy,-hz},{hx,hy,-hz},{-hx,hy,-hz},
        {-hx,-hy, hz},{hx,-hy, hz},{hx,hy, hz},{-hx,hy, hz},
    };
    f32 wv[8][3];
    f32 rotated_normals[6][3];
    f32 origin_x, origin_y, origin_z;
    int i;

    for (i = 0; i < 8; i++)
        mat4_transform_point(world, lv[i][0], lv[i][1], lv[i][2],
                              &wv[i][0], &wv[i][1], &wv[i][2]);

    mat4_transform_point(world, 0.0f, 0.0f, 0.0f, &origin_x, &origin_y, &origin_z);
    for (i = 0; i < 6; i++) {
        f32 tx, ty, tz;
        mat4_transform_point(world, FACE_NORMALS[i][0], FACE_NORMALS[i][1], FACE_NORMALS[i][2],
                              &tx, &ty, &tz);
        rotated_normals[i][0] = tx - origin_x;
        rotated_normals[i][1] = ty - origin_y;
        rotated_normals[i][2] = tz - origin_z;
    }

    emit_box_faces(vl, wv, rotated_normals, r, g, b);
}

static void push_quad_y(VertexList *vl, f32 y, f32 x0, f32 z0, f32 x1, f32 z1,
                        f32 normal_y, f32 r, f32 g, f32 b)
{
    vlist_push(vl, x0, y, z0, 0.0f, normal_y, 0.0f, r, g, b);
    vlist_push(vl, x1, y, z0, 0.0f, normal_y, 0.0f, r, g, b);
    vlist_push(vl, x1, y, z1, 0.0f, normal_y, 0.0f, r, g, b);
    vlist_push(vl, x0, y, z0, 0.0f, normal_y, 0.0f, r, g, b);
    vlist_push(vl, x1, y, z1, 0.0f, normal_y, 0.0f, r, g, b);
    vlist_push(vl, x0, y, z1, 0.0f, normal_y, 0.0f, r, g, b);
}

static void wall_color(int t, f32 *r, f32 *g, f32 *b)
{
    switch (t) {
        case 1: *r = 0.55f; *g = 0.55f; *b = 0.58f; break; /* grey  */
        case 2: *r = 0.25f; *g = 0.35f; *b = 0.85f; break; /* blue  */
        case 3: *r = 0.75f; *g = 0.20f; *b = 0.20f; break; /* red   */
        case 4: *r = 0.20f; *g = 0.65f; *b = 0.30f; break; /* green */
        default: *r = 1.0f; *g = 0.0f; *b = 1.0f; break;   /* shouldn't
                                                             * happen --
                                                             * magenta
                                                             * flags a bug */
    }
}

static int g_ramp_visited[MAP_ROWS][MAP_W];

static int try_chain_axis(int query_mx, int query_my, int dx, int dy,
                          int chain_mx[], int chain_my[], int *out_index, int *out_reversed)
{
    int lo_mx = query_mx, lo_my = query_my, cx, cy, n = 0, i;
    int before_tile, after_tile;

    while (map_tile(lo_mx - dx, lo_my - dy) == TILE_RAMP) {
        lo_mx -= dx; lo_my -= dy;
    }

    cx = lo_mx; cy = lo_my;
    while (map_tile(cx, cy) == TILE_RAMP && n < 24) {
        chain_mx[n] = cx; chain_my[n] = cy;
        n++;
        cx += dx; cy += dy;
    }

    before_tile = map_tile(lo_mx - dx, lo_my - dy);
    after_tile  = map_tile(cx, cy);

    if (before_tile == 0 && after_tile == TILE_PLATFORM) {
        if (out_reversed) *out_reversed = 0;
    } else if (before_tile == TILE_PLATFORM && after_tile == 0) {
        if (out_reversed) *out_reversed = 1;
    } else {
        return 0;
    }

    if (out_index) {
        *out_index = -1;
        for (i = 0; i < n; i++) {
            if (chain_mx[i] == query_mx && chain_my[i] == query_my) { *out_index = i; break; }
        }
    }
    return n;
}

static int find_ramp_chain(int query_mx, int query_my, int chain_mx[], int chain_my[],
                           int *out_index, int *out_dx, int *out_dy, int *out_reversed)
{
    int n;

    n = try_chain_axis(query_mx, query_my, 1, 0, chain_mx, chain_my, out_index, out_reversed);
    if (n > 0) {
        if (out_dx) *out_dx = 1;
        if (out_dy) *out_dy = 0;
        return n;
    }

    n = try_chain_axis(query_mx, query_my, 0, 1, chain_mx, chain_my, out_index, out_reversed);
    if (n > 0) {
        if (out_dx) *out_dx = 0;
        if (out_dy) *out_dy = 1;
        return n;
    }

    return 0;
}

static void build_ramp_chain(VertexList *vl, int start_mx, int start_my)
{
    int chain_mx[24], chain_my[24];
    int dx = 0, dy = 0, reversed = 0;
    int n = find_ramp_chain(start_mx, start_my, chain_mx, chain_my, NULL, &dx, &dy, &reversed);
    int i;
    f32 step;

    if (n == 0) {
        fprintf(stderr, "[level] Ramp at (%d,%d) isn't part of a straight "
                        "chain from open floor to a platform tile -- "
                        "skipping.\n", start_mx, start_my);
        g_ramp_visited[start_my][start_mx] = 1;
        return;
    }
    for (i = 0; i < n; i++) g_ramp_visited[chain_my[i]][chain_mx[i]] = 1;
    if (n < 3) {
        fprintf(stderr, "[level] Ramp chain at (%d,%d) is only %d tile(s) "
                        "-- likely steeper than a comfortable walkable "
                        "slope. Use at least 3.\n", start_mx, start_my, n);
    }

    step = PLATFORM_HEIGHT / (f32)n;
    for (i = 0; i < n; i++) {
        f32 h_start = (reversed ? (f32)(n - i)     : (f32)i)       * step;
        f32 h_end   = (reversed ? (f32)(n - i - 1) : (f32)(i + 1)) * step;
        f32 rise   = h_end - h_start;
        f32 run    = TILE_SIZE;
        f32 angle  = atan2f(rise, run);
        f32 slope_len = sqrtf(run * run + rise * rise);
        f32 center_x = (chain_mx[i] + 0.5f) * TILE_SIZE;
        f32 center_z = (chain_my[i] + 0.5f) * TILE_SIZE;
        f32 center_y = (h_start + h_end) * 0.5f;
        Mat4 world, rot;
        f32 hx, hy, hz;

        if (dx != 0) {
            rot = mat4_rotate_z(angle);
            hx = slope_len * 0.5f; hy = RAMP_THICKNESS * 0.5f; hz = TILE_SIZE * 0.5f;
        } else {
            rot = mat4_rotate_x(-angle);
            hx = TILE_SIZE * 0.5f; hy = RAMP_THICKNESS * 0.5f; hz = slope_len * 0.5f;
        }
        world = mat4_multiply(mat4_translate(center_x, center_y, center_z), rot);
        push_box_transformed(vl, world, hx, hy, hz, 0.5f, 0.4f, 0.25f);
    }
}

f32 level_continuous_ramp_height(f32 x, f32 y)
{
    int mx = (int)x, my = (int)y;
    int chain_mx[24], chain_my[24], index = -1, dx = 0, dy = 0, reversed = 0;
    int n = find_ramp_chain(mx, my, chain_mx, chain_my, &index, &dx, &dy, &reversed);
    f32 frac, effective_frac;

    if (n <= 0 || index < 0) return PLATFORM_HEIGHT * 0.5f;   /* malformed chain -- flat fallback */

    frac = (dx != 0) ? (x - (f32)chain_mx[0]) : (y - (f32)chain_my[0]);
    if (frac < 0.0f) frac = 0.0f;
    if (frac > (f32)n) frac = (f32)n;

    effective_frac = reversed ? ((f32)n - frac) : frac;

    return (effective_frac / (f32)n) * PLATFORM_HEIGHT;
}

VertexList build_level_geometry(void)
{
    VertexList vl;
    int mx, my;

    memset(&vl, 0, sizeof(vl));

    /* Floor + ceiling */
    {
        f32 w = MAP_W * TILE_SIZE, d = MAP_ROWS * TILE_SIZE;
        push_quad_y(&vl, 0.0f,        0.0f, 0.0f, w, d,  1.0f, 0.30f, 0.20f, 0.12f);
        push_quad_y(&vl, WALL_HEIGHT, 0.0f, 0.0f, w, d, -1.0f, 0.08f, 0.09f, 0.14f);
    }

    /* Walls */
    for (my = 0; my < MAP_ROWS; my++) {
        for (mx = 0; mx < MAP_W; mx++) {
            int t = map_tile(mx, my);
            f32 r, g, b;
            if (t < 1 || t > 4) continue;
            wall_color(t, &r, &g, &b);
            push_box(&vl,
                     (mx + 0.5f) * TILE_SIZE, WALL_HEIGHT * 0.5f, (my + 0.5f) * TILE_SIZE,
                     TILE_SIZE * 0.5f, WALL_HEIGHT * 0.5f, TILE_SIZE * 0.5f,
                     r, g, b);
        }
    }

    /* Platforms */
    for (my = 0; my < MAP_ROWS; my++) {
        for (mx = 0; mx < MAP_W; mx++) {
            if (map_tile(mx, my) != TILE_PLATFORM) continue;
            push_box(&vl,
                     (mx + 0.5f) * TILE_SIZE,
                     PLATFORM_HEIGHT - PLATFORM_THICKNESS * 0.5f,
                     (my + 0.5f) * TILE_SIZE,
                     TILE_SIZE * 0.5f, PLATFORM_THICKNESS * 0.5f, TILE_SIZE * 0.5f,
                     0.55f, 0.55f, 0.6f);
        }
    }

    /* Ramps */
    memset(g_ramp_visited, 0, sizeof(g_ramp_visited));
    for (my = 0; my < MAP_ROWS; my++) {
        for (mx = 0; mx < MAP_W; mx++) {
            if (map_tile(mx, my) != TILE_RAMP) continue;
            if (g_ramp_visited[my][mx]) continue;
            build_ramp_chain(&vl, mx, my);
        }
    }

    printf("[level] Built %d vertices (%d triangles)\n", vl.count, vl.count / 3);
    return vl;
}

f32 level_height_for_tile(int mx, int my)
{
    int t = map_tile(mx, my);
    if (t == TILE_PLATFORM) return PLATFORM_HEIGHT;
    if (t == TILE_RAMP)     return level_continuous_ramp_height((f32)mx + 0.5f, (f32)my + 0.5f);
    return 0.0f;
}
