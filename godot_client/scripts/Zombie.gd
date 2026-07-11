extends Node3D
class_name Zombie
## One instance per active zombie the server reports. Purely visual --
## the server owns AI, health, and hit detection entirely.
##
## Swap the CapsuleMesh + material below for an imported sprite/model
## later; nothing else needs to change since everything reads
## update_from_state(). The floating health bar is plain billboarded
## quads (no textures), same "zero asset pipeline risk" philosophy as
## the rest of the level.
##
## Note on death: this node doesn't need to hide itself when the
## zombie dies -- Game.gd's _sync_zombies() already queue_free()s the
## whole Zombie node the instant the server stops reporting that id
## (which happens the same tick it dies), and the health bar is a
## child of this node, so it's torn down along with everything else
## automatically.

const LevelBuilderScript := preload("res://scripts/LevelBuilder.gd")

const HEIGHT := 1.8
const LERP_SPEED := 8.0

# COUPLING WARNING: must match src/server.c's ZOMBIE_HEALTH, or the
# health bar / damage-tint will read wrong percentages even though the
# actual gameplay values on the server are correct.
const MAX_HEALTH := 50.0

const BAR_WIDTH  := 0.6
const BAR_HEIGHT := 0.08
const BAR_Y_OFFSET := HEIGHT + 0.25

var zombie_id: int = -1
var _target_pos: Vector3 = Vector3.ZERO
var _mesh_inst: MeshInstance3D
var _mat: StandardMaterial3D

var _health_fill: MeshInstance3D
var _health_fill_mat: StandardMaterial3D

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

	_build_health_bar()

func _build_health_bar() -> void:
	var bar_root := Node3D.new()
	bar_root.position = Vector3(0, BAR_Y_OFFSET, 0)
	add_child(bar_root)

	# Background -- fixed size, dark, always full width.
	var bg_mesh := QuadMesh.new()
	bg_mesh.size = Vector2(BAR_WIDTH, BAR_HEIGHT)
	var bg_mat := StandardMaterial3D.new()
	bg_mat.albedo_color = Color(0.08, 0.08, 0.08)
	bg_mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	bg_mat.billboard_mode = BaseMaterial3D.BILLBOARD_ENABLED
	var bg_inst := MeshInstance3D.new()
	bg_inst.mesh = bg_mesh
	bg_inst.material_override = bg_mat
	bar_root.add_child(bg_inst)

	# Fill -- shrinks from the right edge as health drops. Nudged
	# slightly toward the camera side so it doesn't z-fight the
	# background (two coplanar billboarded quads otherwise flicker).
	var fill_mesh := QuadMesh.new()
	fill_mesh.size = Vector2(BAR_WIDTH, BAR_HEIGHT)
	_health_fill_mat = StandardMaterial3D.new()
	_health_fill_mat.albedo_color = Color(0.2, 0.9, 0.2)
	_health_fill_mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	_health_fill_mat.billboard_mode = BaseMaterial3D.BILLBOARD_ENABLED
	_health_fill = MeshInstance3D.new()
	_health_fill.mesh = fill_mesh
	_health_fill.material_override = _health_fill_mat
	_health_fill.position = Vector3(0, 0.001, 0)
	bar_root.add_child(_health_fill)

func update_from_state(state: Dictionary) -> void:
	_target_pos = LevelBuilderScript.map_to_world(state["x"], state["y"], state["z"])
	visible = state["alive"]

	var t: float = clamp(float(state["health"]) / MAX_HEALTH, 0.0, 1.0)

	# Darken the body toward black as health drops (kept a nonzero
	# floor so it doesn't go fully black while still alive).
	_mat.albedo_color = Color(0.25, 0.55, 0.2) * clamp(t, 0.15, 1.0)

	# Resize the fill bar and keep its LEFT edge fixed while its right
	# edge recedes -- QuadMesh is centered by default, so shrinking the
	# size alone would shrink from the middle instead.
	var fill_width: float = max(BAR_WIDTH * t, 0.001)
	var fill_mesh: QuadMesh = _health_fill.mesh
	fill_mesh.size = Vector2(fill_width, BAR_HEIGHT)
	_health_fill.position.x = -(BAR_WIDTH - fill_width) / 2.0

	# Green -> yellow -> red as health drops, same idea as the HUD's
	# player health color thresholds.
	if t > 0.5:
		_health_fill_mat.albedo_color = Color(0.2, 0.9, 0.2)
	elif t > 0.25:
		_health_fill_mat.albedo_color = Color(0.9, 0.7, 0.1)
	else:
		_health_fill_mat.albedo_color = Color(0.9, 0.15, 0.15)

func _process(delta: float) -> void:
	global_position = global_position.lerp(_target_pos, min(1.0, LERP_SPEED * delta))
