extends Node
class_name MapData
## Autoload-free helper (used as a regular class, not a singleton).
## Mirrors src/map.c exactly: 0=floor, 1=grey/2=blue/3=red/4=green wall,
## 5=pickup spawn marker, 6=ramp, 7=elevated platform. Keep this in
## sync if map.c ever changes -- this GRID had drifted out of sync
## with the pickup markers your friend added (harmless so far, since
## Pickup.gd renders from server-reported positions, not from this
## grid, but is_wall() below was also wrong until now).

const MAP_W := 24
const MAP_ROWS := 24

const TILE_PICKUP_SPAWN := 5
const TILE_RAMP         := 6
const TILE_PLATFORM     := 7

const GRID: Array = [
	[1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1],
	[1,0,0,5,0,0,0,0,0,0,0,5,0,0,0,0,0,0,0,0,5,0,0,1],
	[1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1],
	[1,0,0,0,0,0,2,2,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1],
	[1,0,0,0,0,0,2,0,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1],
	[1,0,0,0,0,0,2,0,2,0,0,0,0,3,3,3,3,3,0,0,0,0,0,1],
	[1,0,0,0,0,0,2,0,0,0,0,0,0,3,0,0,0,3,0,0,0,0,0,1],
	[1,0,0,0,0,0,2,2,2,0,0,0,0,3,0,0,0,3,0,0,0,0,0,1],
	[1,0,0,0,0,0,0,0,0,0,0,0,0,3,0,0,0,3,0,0,0,0,0,1],
	[1,0,0,0,0,0,0,0,0,0,0,0,0,3,3,0,3,3,0,0,7,7,0,1],
	[1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,6,6,6,7,7,0,1],
	[1,0,0,5,0,0,0,0,0,0,0,5,0,0,0,0,0,0,0,0,5,0,0,1],
	[1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1],
	[1,0,0,0,4,4,4,4,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1],
	[1,0,0,0,4,0,0,0,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1],
	[1,0,0,0,4,0,0,0,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1],
	[1,0,0,0,4,0,0,0,4,0,0,0,0,0,0,4,4,4,4,0,0,0,0,1],
	[1,0,0,0,4,4,0,4,4,0,0,0,0,0,0,4,0,0,4,0,0,0,0,1],
	[1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,4,0,0,4,0,0,0,0,1],
	[1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,4,4,4,4,0,0,0,0,1],
	[1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1],
	[1,0,0,5,0,0,0,0,0,0,0,5,0,0,0,0,0,0,0,0,5,0,0,1],
	[1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1],
	[1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1],
]

const WALL_COLORS := {
	1: Color(0.55, 0.55, 0.58),  # grey
	2: Color(0.25, 0.35, 0.85),  # blue
	3: Color(0.75, 0.20, 0.20),  # red
	4: Color(0.20, 0.65, 0.30),  # green
}

static func is_wall(mx: int, my: int) -> bool:
	if mx < 0 or mx >= MAP_W or my < 0 or my >= MAP_ROWS:
		return true
	var t: int = GRID[my][mx]
	return t >= 1 and t <= 4   # only wall types block movement, matching map.c's map_is_wall

static func tile(mx: int, my: int) -> int:
	if mx < 0 or mx >= MAP_W or my < 0 or my >= MAP_ROWS:
		return 1
	return GRID[my][mx]
