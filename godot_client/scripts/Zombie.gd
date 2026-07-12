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

# ZOMBIE_TYPE_* -- must match net.h's ZOMBIE_TYPE_NORMAL/_TANK/_BOSS.
const TYPE_NORMAL := 0
const TYPE_TANK   := 1
const TYPE_BOSS   := 2

# Per-type body height and radius. Base HEIGHT/RADIUS match the
# original normal-zombie size; tank and boss scale up from there so
# the mini-boss (red) and final boss (blue) both read as visually
# bigger threats at a glance.
const BASE_HEIGHT := 1.8
const BASE_RADIUS := 0.35
const TANK_SCALE := 1.3    # "a bit bigger" than normal
const BOSS_SCALE := 1.7    # bigger than the tank

# Base body colors per type -- green/red/blue so all three are
# unmistakable from across the map, even before the health bar tints in.
const COLOR_NORMAL := Color(0.25, 0.55, 0.2)   # sickly green
const COLOR_TANK   := Color(0.75, 0.15, 0.15)  # red -- the mini-boss
const COLOR_BOSS   := Color(0.15, 0.35, 0.85)  # blue -- the final boss

const LERP_SPEED := 8.0

const BAR_WIDTH  := 0.6
const BAR_HEIGHT := 0.08

var zombie_id: int = -1
var zombie_type: int = TYPE_NORMAL
var _height: float = BASE_HEIGHT
var _base_color: Color = COLOR_NORMAL
var _bar_y_offset: float = BASE_HEIGHT + 0.25

var _target_pos: Vector3 = Vector3.ZERO
var _mesh_inst: MeshInstance3D
var _mat: StandardMaterial3D

var _health_fill: MeshInstance3D
var _health_fill_mat: StandardMaterial3D
var _bar_root: Node3D

func _ready() -> void:
	var capsule := CapsuleMesh.new()
	capsule.radius = BASE_RADIUS
	capsule.height = _height
	_mesh_inst = MeshInstance3D.new()
	_mesh_inst.mesh = capsule
	_mesh_inst.position = Vector3(0, _height / 2.0, 0)
	_mat = StandardMaterial3D.new()
	_mat.albedo_color = _base_color
	_mesh_inst.material_override = _mat
	add_child(_mesh_inst)

	_build_health_bar()

## Applies the visual differences for a zombie type: body color, and
## height/radius scale (which also moves the health bar up to stay
## above the taller tank/boss models). Called once per zombie the
## first time its type is known (types never change mid-life on the
## server, so this only actually runs once per zombie in practice).
func _apply_type(type: int) -> void:
	zombie_type = type
	var scale: float = 1.0
	match type:
		TYPE_TANK:
			_base_color = COLOR_TANK
			scale = TANK_SCALE
		TYPE_BOSS:
			_base_color = COLOR_BOSS
			scale = BOSS_SCALE
		_:
			_base_color = COLOR_NORMAL
			scale = 1.0

	_height = BASE_HEIGHT * scale
	_bar_y_offset = _height + 0.25

	if _mesh_inst:
		var capsule: CapsuleMesh = _mesh_inst.mesh
		capsule.radius = BASE_RADIUS * scale
		capsule.height = _height
		_mesh_inst.position = Vector3(0, _height / 2.0, 0)
		_mat.albedo_color = _base_color
	if _bar_root:
		_bar_root.position = Vector3(0, _bar_y_offset, 0)

func _build_health_bar() -> void:
	_bar_root = Node3D.new()
	_bar_root.position = Vector3(0, _bar_y_offset, 0)
	add_child(_bar_root)

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
	_bar_root.add_child(bg_inst)

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
	_bar_root.add_child(_health_fill)

func update_from_state(state: Dictionary) -> void:
	_target_pos = LevelBuilderScript.map_to_world(state["x"], state["y"], state["z"])
	visible = state["alive"]

	if state["type"] != zombie_type:
		_apply_type(state["type"])

	# health_max comes from the server now (per-type: normal/tank/boss
	# have very different HP pools), so the bar percentage is correct
	# regardless of which type this is -- no more hardcoded MAX_HEALTH
	# that could silently drift out of sync with server.c.
	var health_max: float = max(float(state["health_max"]), 1.0)
	var t: float = clamp(float(state["health"]) / health_max, 0.0, 1.0)

	# Darken the body toward black as health drops (kept a nonzero
	# floor so it doesn't go fully black while still alive).
	_mat.albedo_color = _base_color * clamp(t, 0.15, 1.0)

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
