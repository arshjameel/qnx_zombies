extends Node3D
class_name Zombie
const LevelBuilderScript := preload("res://scripts/LevelBuilder.gd")

const HEIGHT := 1.8
const LERP_SPEED := 8.0

var zombie_id: int = -1
var _target_pos: Vector3 = Vector3.ZERO
var _mesh_inst: MeshInstance3D
var _mat: StandardMaterial3D

func _ready() -> void:
	var capsule := CapsuleMesh.new()
	capsule.radius = 0.35
	capsule.height = HEIGHT
	_mesh_inst = MeshInstance3D.new()
	_mesh_inst.mesh = capsule
	_mesh_inst.position = Vector3(0, HEIGHT / 2.0, 0)
	_mat = StandardMaterial3D.new()
	_mat.albedo_color = Color(0.25, 0.55, 0.2)   # sickly green
	_mesh_inst.material_override = _mat
	add_child(_mesh_inst)

func update_from_state(state: Dictionary) -> void:
	_target_pos = LevelBuilderScript.map_to_world(state["x"], state["y"], 0.0)
	visible = state["alive"]
	# Darken toward black as health drops, cheap "damaged" feedback
	# without needing any imported assets.
	var t: float = clamp(float(state["health"]) / 50.0, 0.15, 1.0)
	_mat.albedo_color = Color(0.25, 0.55, 0.2) * t

func _process(delta: float) -> void:
	global_position = global_position.lerp(_target_pos, min(1.0, LERP_SPEED * delta))
