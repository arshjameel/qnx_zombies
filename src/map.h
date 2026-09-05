#ifndef MAP_H
#define MAP_H

#include "common.h"

#define MAP_W    24
#define MAP_ROWS 24
#define TILE_PICKUP_SPAWN 5
#define TILE_RAMP      6
#define TILE_PLATFORM  7
#define MAX_PICKUP_SPAWNS 16

extern const u8 g_map[MAP_ROWS][MAP_W];

int map_is_wall(int mx, int my);
int map_tile(int mx, int my);
int map_get_pickup_spawns(f32 *out_x, f32 *out_y);

#endif 