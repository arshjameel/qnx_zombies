#include "map.h"
#include <string.h>

/*
 * 24x24 tile map.  0 = floor, 1-4 = wall types, 5 = pickup spawn marker,
 * 6 = ramp, 7 = elevated platform (all of 0/5/6/7 are walkable floor --
 * see map_is_wall). Outer ring is always solid.
 *
 * The ramp at row 10 (cols 17-19) climbs up to the 2x2 platform at
 * rows 9-10, cols 20-21 -- a small worked example of the
 * ramp-chain-must-be-straight-with-floor-at-one-end-and-platform-at-
 * the-other rule described in map.h.
 */
const u8 g_map[MAP_ROWS][MAP_W] = {
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
    {1,0,0,5,0,0,0,0,0,0,0,5,0,0,0,0,0,0,0,0,5,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,6,0,0,6,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,6,0,0,6,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,6,0,0,6,0,0,0,0,0,0,7,7,0,1},
    {1,0,0,0,0,0,0,6,6,6,7,7,7,7,6,6,6,0,0,0,7,7,0,1},
    {1,0,0,5,0,0,0,0,0,0,7,7,7,7,0,0,0,0,0,0,5,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,7,7,7,7,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,6,6,6,7,7,7,7,6,6,6,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,6,0,0,6,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,6,0,0,6,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,6,0,0,6,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,5,0,0,0,0,0,0,0,5,0,0,0,0,0,0,0,0,5,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
};

int map_is_wall(int mx, int my)
{
    int t;
    if (mx < 0 || mx >= MAP_W || my < 0 || my >= MAP_ROWS) return 1;
    t = (int)g_map[my][mx];
    return (t >= 1 && t <= 4);   /* only wall types block movement; 5 (pickup marker) is floor */
}

int map_tile(int mx, int my)
{
    if (mx < 0 || mx >= MAP_W || my < 0 || my >= MAP_ROWS) return 1;
    return (int)g_map[my][mx];
}

int map_get_pickup_spawns(f32 *out_x, f32 *out_y)
{
    int mx, my, count = 0;
    for (my = 0; my < MAP_ROWS && count < MAX_PICKUP_SPAWNS; my++) {
        for (mx = 0; mx < MAP_W && count < MAX_PICKUP_SPAWNS; mx++) {
            if (g_map[my][mx] != TILE_PICKUP_SPAWN) continue;
            out_x[count] = (f32)mx + 0.5f;
            out_y[count] = (f32)my + 0.5f;
            count++;
        }
    }
    return count;
}