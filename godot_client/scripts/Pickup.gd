extends Node3D
class_name Pickup
## Visual representation of one ammo/health pickup slot. The server
## owns everything about when it's active vs. on cooldown and applies
## the actual effect the instant a player walks over it -- this node
## just shows/hides accordingly and spins a little for "collectible"
## flair. Pickups don't move, so there's no lerp like Zombie/RemotePlayer
## have; position is set once and only visibility/color change after.

const LevelBuilderScript := preload("res://scripts/LevelBuilder.gd")

const FLOAT_HEIGHT := 0.9
const SPIN_SPEED := 1.5   # radians/sec, purely cosmetic

var pickup_id: int = -1
var _mesh_inst: MeshInstance3D
var _mat: StandardMaterial3D

func _ready() -> void:
	var box := BoxMesh.new()
	box.size = Vector3(0.35, 0.35, 0.35)
	_mesh_inst = MeshInstance3D.new()
	_mesh_inst.mesh = box
	_mesh_inst.position = Vector3(0, FLOAT_HEIGHT, 0)
	_mat = StandardMaterial3D.new()
	_mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	_mesh_inst.material_override = _mat
	add_child(_mesh_inst)

func update_from_state(state: Dictionary) -> void:
	# Pickups are stationary (map-fixed spawn points), so just snap
	# position directly rather than lerping toward it every frame.
	global_position = LevelBuilderScript.map_to_world(state["x"], state["y"], 0.0)
	visible = state["active"]
	if state["type"] == Network.PICKUP_AMMO:
		_mat.albedo_color = Color(0.9, 0.7, 0.15)    # amber/brass
	else:
		_mat.albedo_color = Color(0.85, 0.15, 0.2)   # red cross-ish

func _process(delta: float) -> void:
	_mesh_inst.rotate_y(SPIN_SPEED * delta)
