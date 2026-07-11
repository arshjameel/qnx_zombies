extends Node3D
class_name LevelBuilder
## Attach to an empty Node3D in the Game scene. Generates blocky,
## Doom-ish geometry from MapData at runtime -- no imported level
## assets needed, which keeps this fast to iterate on and easy to
## export to every target (Linux, Windows, QNX) with zero asset
## pipeline risk.
##
## Ramps/platforms (tiles 6/7) are a CLIENT-ONLY feature. The server
## has zero concept of height -- it only ever tracked (x, y) -- so:
##   - A zombie standing directly "under" an elevated player is, as
##     far as the server's math is concerned, at the same position as
##     that player, so it can still land melee hits on someone who is
##     actually standing up on a platform. Known quirk, not a bug --
##     giving the server real verticality would be a much bigger
##     project than this.
##   - src/server.c's classify_headshot() assumes a fixed eye height
##     relative to floor level, so headshots fired down from a
##     platform will be misclassified for the same reason.
## Both are acceptable tradeoffs for a client-visual/physics-only
## verticality feature -- just don't expect the AI or hit detection to
## know it exists.

const MapDataScript := preload("res://scripts/MapData.gd")

const TILE_SIZE := 1.0
const WALL_HEIGHT := 4.0   # raised from 3.0 -- leaves headroom both
                           # under and standing on top of a platform

const PLATFORM_HEIGHT    := 2.0    # walking-surface height above the floor
const PLATFORM_THICKNESS := 0.25
const RAMP_THICKNESS     := 0.3

func _ready() -> void:
	_build_floor_ceiling()
	_build_walls()
	_build_platforms()
	_build_ramps()

func _build_floor_ceiling() -> void:
	var w := MapDataScript.MAP_W * TILE_SIZE
	var d := MapDataScript.MAP_ROWS * TILE_SIZE
	var cx := w / 2.0
	var cz := d / 2.0

	var floor_mesh := PlaneMesh.new()
	floor_mesh.size = Vector2(w, d)
	var floor_inst := MeshInstance3D.new()
	floor_inst.mesh = floor_mesh
	floor_inst.position = Vector3(cx, 0.0, cz)
	var floor_mat := StandardMaterial3D.new()
	floor_mat.albedo_color = Color(0.30, 0.20, 0.12)
	floor_inst.material_override = floor_mat
	add_child(floor_inst)

	var floor_body := StaticBody3D.new()
	var floor_shape := CollisionShape3D.new()
	var box := BoxShape3D.new()
	box.size = Vector3(w, 0.1, d)
	floor_shape.shape = box
	floor_body.position = Vector3(cx, -0.05, cz)
	floor_body.add_child(floor_shape)
	add_child(floor_body)

	var ceil_mesh := PlaneMesh.new()
	ceil_mesh.size = Vector2(w, d)
	var ceil_inst := MeshInstance3D.new()
	ceil_inst.mesh = ceil_mesh
	ceil_inst.position = Vector3(cx, WALL_HEIGHT, cz)
	ceil_inst.rotation_degrees = Vector3(180, 0, 0)
	var ceil_mat := StandardMaterial3D.new()
	ceil_mat.albedo_color = Color(0.08, 0.09, 0.14)
	ceil_inst.material_override = ceil_mat
	add_child(ceil_inst)

func _build_walls() -> void:
	# Materials cached per wall type so we don't create one per tile.
	var materials := {}
	for tile_val in MapDataScript.WALL_COLORS.keys():
		var mat := StandardMaterial3D.new()
		mat.albedo_color = MapDataScript.WALL_COLORS[tile_val]
		materials[tile_val] = mat

	var box_mesh := BoxMesh.new()
	box_mesh.size = Vector3(TILE_SIZE, WALL_HEIGHT, TILE_SIZE)
	var box_shape := BoxShape3D.new()
	box_shape.size = Vector3(TILE_SIZE, WALL_HEIGHT, TILE_SIZE)

	for my in range(MapDataScript.MAP_ROWS):
		for mx in range(MapDataScript.MAP_W):
			var t: int = MapDataScript.tile(mx, my)
			# Only tiles with an actual wall color (1-4) get a generic
			# wall box. Everything else -- floor, pickup markers,
			# ramps, platforms -- is walkable and handled elsewhere
			# (or not at all, for plain floor). This used to only
			# check `t == 0`, which meant pickup markers were getting
			# entombed in a solid grey wall box.
			if not MapDataScript.WALL_COLORS.has(t):
				continue
			var pos := Vector3(
				(mx + 0.5) * TILE_SIZE,
				WALL_HEIGHT / 2.0,
				(my + 0.5) * TILE_SIZE
			)

			var body := StaticBody3D.new()
			body.position = pos

			var mesh_inst := MeshInstance3D.new()
			mesh_inst.mesh = box_mesh
			mesh_inst.material_override = materials[t]
			body.add_child(mesh_inst)

			var shape_inst := CollisionShape3D.new()
			shape_inst.shape = box_shape
			body.add_child(shape_inst)

			add_child(body)

## One flat slab per TILE_PLATFORM tile, top surface exactly at
## PLATFORM_HEIGHT. Adjacent platform tiles are built individually
## rather than merged into one big slab -- simpler code, and the seams
## between tiles aren't noticeable in practice.
func _build_platforms() -> void:
	var mat := StandardMaterial3D.new()
	mat.albedo_color = Color(0.55, 0.55, 0.6)   # steel-ish -- reads as "structure", not ground

	var box_mesh := BoxMesh.new()
	box_mesh.size = Vector3(TILE_SIZE, PLATFORM_THICKNESS, TILE_SIZE)
	var box_shape := BoxShape3D.new()
	box_shape.size = box_mesh.size

	for my in range(MapDataScript.MAP_ROWS):
		for mx in range(MapDataScript.MAP_W):
			if MapDataScript.tile(mx, my) != MapDataScript.TILE_PLATFORM:
				continue

			var body := StaticBody3D.new()
			body.position = Vector3(
				(mx + 0.5) * TILE_SIZE,
				PLATFORM_HEIGHT - PLATFORM_THICKNESS / 2.0,
				(my + 0.5) * TILE_SIZE
			)

			var mesh_inst := MeshInstance3D.new()
			mesh_inst.mesh = box_mesh
			mesh_inst.material_override = mat
			body.add_child(mesh_inst)

			var shape_inst := CollisionShape3D.new()
			shape_inst.shape = box_shape
			body.add_child(shape_inst)

			add_child(body)

# ---------------------------------------------------------------------
# Ramps
#
# A ramp is a straight, unbranched run of TILE_RAMP tiles with open
# floor (0) at the low end and a TILE_PLATFORM tile at the high end --
# see the authoring rule documented in map.h. Detected once per chain
# (flood-fill along whichever axis is straight), then built as one
# rotated box per tile so the chain climbs smoothly from 0 up to
# PLATFORM_HEIGHT.
# ---------------------------------------------------------------------
var _ramp_visited: Dictionary = {}

func _build_ramps() -> void:
	_ramp_visited.clear()
	for my in range(MapDataScript.MAP_ROWS):
		for mx in range(MapDataScript.MAP_W):
			if MapDataScript.tile(mx, my) != MapDataScript.TILE_RAMP:
				continue
			var key := "%d,%d" % [mx, my]
			if _ramp_visited.has(key):
				continue
			_build_ramp_chain(mx, my)

## Walks a SPECIFIC candidate axis (dx,dy fixed to one of (1,0)/(0,1))
## starting from (start_mx, start_my), and reports whether that axis
## produces a valid chain. Returns {} (invalid) rather than guessing --
## the caller (_find_ramp_chain) tries both axes and uses whichever one
## actually validates, since checking immediate-neighbor walkability
## alone can't reliably tell direction apart near a junction where
## multiple ramps converge on the same platform (every such tile has
## walkable neighbors on BOTH axes, since the surrounding area is open
## floor either way).
##
## Also detects REVERSED chains: valid whether the low-dx/dy end is
## open floor and the high end is the platform (normal), OR the low
## end is the platform and the high end is open floor (reversed) --
## both are geometrically valid ramps, just sloping in opposite
## directions relative to the chain's own index order. Which case
## applies depends on where the platform sits relative to the ramp,
## not on which tile the query started from.
func _try_chain_axis(start_mx: int, start_my: int, dx: int, dy: int) -> Dictionary:
	var lo_mx := start_mx
	var lo_my := start_my
	while MapDataScript.tile(lo_mx - dx, lo_my - dy) == MapDataScript.TILE_RAMP:
		lo_mx -= dx
		lo_my -= dy

	var chain: Array = []
	var cx := lo_mx
	var cy := lo_my
	while MapDataScript.tile(cx, cy) == MapDataScript.TILE_RAMP:
		chain.append(Vector2i(cx, cy))
		cx += dx
		cy += dy

	var before_tile: int = MapDataScript.tile(lo_mx - dx, lo_my - dy)
	var after_tile: int = MapDataScript.tile(cx, cy)

	if before_tile == 0 and after_tile == MapDataScript.TILE_PLATFORM:
		return {"chain": chain, "reversed": false}
	elif before_tile == MapDataScript.TILE_PLATFORM and after_tile == 0:
		return {"chain": chain, "reversed": true}
	else:
		return {}

## Finds the full straight ramp chain containing (start_mx, start_my),
## trying horizontal then vertical and using whichever actually
## validates. Returns {} if neither axis produces a valid chain, or
## {"chain": Array[Vector2i], "dx": int, "dy": int, "reversed": bool}.
func _find_ramp_chain(start_mx: int, start_my: int) -> Dictionary:
	var result := _try_chain_axis(start_mx, start_my, 1, 0)
	if not result.is_empty():
		result["dx"] = 1
		result["dy"] = 0
		return result

	result = _try_chain_axis(start_mx, start_my, 0, 1)
	if not result.is_empty():
		result["dx"] = 0
		result["dy"] = 1
		return result

	return {}

func _build_ramp_chain(start_mx: int, start_my: int) -> void:
	var result := _find_ramp_chain(start_mx, start_my)
	if result.is_empty():
		push_warning("Ramp at (%d,%d) isn't part of a straight chain from open floor to a platform tile -- skipping." % [start_mx, start_my])
		_ramp_visited["%d,%d" % [start_mx, start_my]] = true
		return

	var chain: Array = result["chain"]
	var dx: int = result["dx"]
	var dy: int = result["dy"]
	var reversed: bool = result["reversed"]

	for c in chain:
		_ramp_visited["%d,%d" % [c.x, c.y]] = true

	var n := chain.size()
	if n < 3:
		push_warning("Ramp chain at (%d,%d) is only %d tile(s) long -- likely steeper than Godot's default 45-degree walkable slope limit. Use at least 3 tiles for a comfortable incline." % [start_mx, start_my, n])

	var step := PLATFORM_HEIGHT / float(n)
	for i in range(n):
		var c: Vector2i = chain[i]
		# Reversed chains have height decreasing as index increases
		# (index 0 sits next to the platform, not the floor) -- signed
		# so _build_ramp_segment's rise = h_high - h_low comes out
		# negative and tilts the segment the correct way, not just
		# "the right two values in the wrong order". Matches the
		# verified h_start/h_end derivation in level_geo.c.
		var h_start: float = (float(n - i) if reversed else float(i)) * step
		var h_end: float = (float(n - i - 1) if reversed else float(i + 1)) * step
		_build_ramp_segment(c.x, c.y, dx, dy, h_start, h_end)

## Builds one sloped segment of a ramp chain, from height h_low (at
## its low edge) to h_high (at its high edge, one tile-length away in
## whichever direction dx/dy indicates is "uphill"). Mesh and collision
## are the same rotated box, so they can never visually disagree with
## each other -- if the ramp looks upside down or tilts the wrong way
## when you actually test it, flip the sign on the rotation line below
## for whichever axis it is (this is hand-derived rotation math I
## haven't been able to test in a running Godot instance, so a sign
## flip here is the most likely thing to need a quick fix).
func _build_ramp_segment(mx: int, my: int, dx: int, dy: int, h_low: float, h_high: float) -> void:
	var rise := h_high - h_low
	var run := TILE_SIZE
	var angle := atan2(rise, run)
	var slope_length := sqrt(run * run + rise * rise)

	var box_mesh := BoxMesh.new()
	var box_shape := BoxShape3D.new()
	var body := StaticBody3D.new()

	if dx != 0:
		box_mesh.size = Vector3(slope_length, RAMP_THICKNESS, TILE_SIZE)
		body.rotation.z = angle
	else:
		box_mesh.size = Vector3(TILE_SIZE, RAMP_THICKNESS, slope_length)
		body.rotation.x = -angle
	box_shape.size = box_mesh.size

	body.position = Vector3(
		(mx + 0.5) * TILE_SIZE,
		(h_low + h_high) / 2.0,
		(my + 0.5) * TILE_SIZE
	)

	var mat := StandardMaterial3D.new()
	mat.albedo_color = Color(0.5, 0.4, 0.25)   # distinct from platform/wall colors

	var mesh_inst := MeshInstance3D.new()
	mesh_inst.mesh = box_mesh
	mesh_inst.material_override = mat
	body.add_child(mesh_inst)

	var shape_inst := CollisionShape3D.new()
	shape_inst.shape = box_shape
	body.add_child(shape_inst)

	add_child(body)

## Map-space (x, y) <-> world-space (x, z) helpers, used everywhere
## entities need to translate between the server's 2D coordinate
## system and Godot's 3D world.
static func map_to_world(mx: float, my: float, y_height: float = 0.0) -> Vector3:
	return Vector3(mx * TILE_SIZE, y_height, my * TILE_SIZE)

static func world_to_map(pos: Vector3) -> Vector2:
	return Vector2(pos.x / TILE_SIZE, pos.z / TILE_SIZE)
