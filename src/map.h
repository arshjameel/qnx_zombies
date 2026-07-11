#ifndef MAP_H
#define MAP_H

#include "common.h"

#define MAP_W    24
#define MAP_ROWS 24

extern const u8 g_map[MAP_ROWS][MAP_W];

/* returns non-zero if tile (mx, my) is a wall */
int map_is_wall(int mx, int my);

/* returns wall type at (mx, my), 0 if floor */
int map_tile(int mx, int my);

#endif
