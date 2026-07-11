extends Node3D
class_name RemotePlayer
const LevelBuilderScript := preload("res://scripts/LevelBuilder.gd")

const HEIGHT := 1.6
const LERP_SPEED := 10.0

var player_id: int = -1
var _target_pos: Vector3 = Vector3.ZERO
var _mesh_inst: MeshInstance3D

func _ready() -> void:
	var capsule := CapsuleMesh.new()
	capsule.radius = 0.3
	capsule.height = HEIGHT
	_mesh_inst = MeshInstance3D.new()
	_mesh_inst.mesh = capsule
	_mesh_inst.position = Vector3(0, HEIGHT / 2.0, 0)
	var mat := StandardMaterial3D.new()
	mat.albedo_color = Color(0.9, 0.75, 0.1)   # yellow -- teammate
	_mesh_inst.material_override = mat
	add_child(_mesh_inst)

func update_from_state(state: Dictionary) -> void:
	_target_pos = LevelBuilderScript.map_to_world(state["x"], state["y"], 0.0)
	visible = state["alive"]

func _process(delta: float) -> void:
	global_position = global_position.lerp(_target_pos, min(1.0, LERP_SPEED * delta))
