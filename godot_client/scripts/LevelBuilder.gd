extends Node3D
class_name LevelBuilder

const MapDataScript := preload("res://scripts/MapData.gd")

const TILE_SIZE := 1.0
const WALL_HEIGHT := 4.0  

const PLATFORM_HEIGHT    := 2.0   
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

func _build_platforms() -> void:
	var mat := StandardMaterial3D.new()
	mat.albedo_color = Color(0.55, 0.55, 0.6)   

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
		var h_start: float = (float(n - i) if reversed else float(i)) * step
		var h_end: float = (float(n - i - 1) if reversed else float(i + 1)) * step
		_build_ramp_segment(c.x, c.y, dx, dy, h_start, h_end)

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
	mat.albedo_color = Color(0.5, 0.4, 0.25)   

	var mesh_inst := MeshInstance3D.new()
	mesh_inst.mesh = box_mesh
	mesh_inst.material_override = mat
	body.add_child(mesh_inst)

	var shape_inst := CollisionShape3D.new()
	shape_inst.shape = box_shape
	body.add_child(shape_inst)

	add_child(body)

static func map_to_world(mx: float, my: float, y_height: float = 0.0) -> Vector3:
	return Vector3(mx * TILE_SIZE, y_height, my * TILE_SIZE)

static func world_to_map(pos: Vector3) -> Vector2:
	return Vector2(pos.x / TILE_SIZE, pos.z / TILE_SIZE)
