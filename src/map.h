#ifndef MAP_H
#define MAP_H

#include "common.h"

#define MAP_W    24
#define MAP_ROWS 24

/* Tile value 5 = pickup spawn marker (ammo/health). It is FLOOR for
 * movement/collision purposes -- only 1-4 are walls. map_is_wall()
 * below already reflects that. */
#define TILE_PICKUP_SPAWN 5

/* Tile 6 = ramp segment, tile 7 = elevated platform floor. Both are
 * FLOOR for the server's (x,y)-only collision -- exactly like the
 * pickup marker, they're walkable, not walls. The server has zero
 * concept of height/Z at all, so ramps/platforms are purely a
 * client-side visual+physics feature (see LevelBuilder.gd); the
 * server just needs to not block movement through their footprint.
 *
 * Ramp authoring rule (enforced by LevelBuilder.gd at load time, not
 * here): ramp tiles must form a straight contiguous run (no turns,
 * no branches) with open floor (0) at the low end and a platform (7)
 * tile at the high end, and should be at least 3 tiles long so the
 * resulting incline stays under Godot's default 45 degree walkable
 * slope limit. Malformed chains are skipped with a warning rather
 * than crashing. */
#define TILE_RAMP      6
#define TILE_PLATFORM  7

#define MAX_PICKUP_SPAWNS 16

extern const u8 g_map[MAP_ROWS][MAP_W];

/* Returns non-zero if tile (mx, my) is a wall (tile types 1-4 only;
 * 0, 5, 6, and 7 are all walkable floor -- see TILE_RAMP/TILE_PLATFORM
 * above for why ramps/platforms are floor as far as the server cares). */
int map_is_wall(int mx, int my);

/* Returns raw tile value at (mx, my): 0=floor, 1-4=wall types,
 * 5=pickup spawn marker, 6=ramp, 7=elevated platform floor. Returns 1
 * (wall) if out of bounds. */
int map_tile(int mx, int my);

/* Scans g_map for TILE_PICKUP_SPAWN markers and fills out_x/out_y with
 * the tile-center world coords of each one found (capped at
 * MAX_PICKUP_SPAWNS). Returns the count found. The server uses this at
 * startup instead of a hand-maintained coordinate list, so editing the
 * map (adding/moving 5's) is all that's needed to change pickup spots. */
int map_get_pickup_spawns(f32 *out_x, f32 *out_y);

#endif /* MAP_H */