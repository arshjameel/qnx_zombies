extends Node3D
class_name LevelBuilder
const MapDataScript := preload("res://scripts/MapData.gd")

const WALL_HEIGHT := 3.0
const TILE_SIZE := 1.0

func _ready() -> void:
	_build_floor_ceiling()
	_build_walls()

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
			if t == 0:
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
			mesh_inst.material_override = materials.get(t, materials[1])
			body.add_child(mesh_inst)

			var shape_inst := CollisionShape3D.new()
			shape_inst.shape = box_shape
			body.add_child(shape_inst)

			add_child(body)

## translate between the server's 2D coordinate system and Godot's 3D world.
static func map_to_world(mx: float, my: float, y_height: float = 0.0) -> Vector3:
	return Vector3(mx * TILE_SIZE, y_height, my * TILE_SIZE)

static func world_to_map(pos: Vector3) -> Vector2:
	return Vector2(pos.x / TILE_SIZE, pos.z / TILE_SIZE)
