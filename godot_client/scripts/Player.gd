extends CharacterBody3D
class_name Player

const LevelBuilderScript := preload("res://scripts/LevelBuilder.gd")

const PLAYER_SPEED := 1.0     
const PLAYER_HEIGHT := 1.6
const INPUT_SEND_HZ := 20.0
const RECONCILE_LERP := 0.35

const GRAVITY := 9.8
const JUMP_VELOCITY := 4.5

const LOOK_SPEED := 2.0

var camera: Camera3D
var pitch: float = 0.0

var health: int = 100
var ammo: int = 30
var alive: bool = true

var _send_accum: float = 0.0

func _ready() -> void:
	var shape := CollisionShape3D.new()
	var capsule := CapsuleShape3D.new()
	capsule.radius = 0.3
	capsule.height = PLAYER_HEIGHT
	shape.shape = capsule
	shape.position = Vector3(0, PLAYER_HEIGHT / 2.0, 0)
	add_child(shape)

	camera = Camera3D.new()
	camera.position = Vector3(0, PLAYER_HEIGHT - 0.2, 0)
	camera.fov = GameState.fov_degrees
	camera.current = true
	add_child(camera)

	Input.mouse_mode = Input.MOUSE_MODE_CAPTURED
	Network.state_updated.connect(_on_state_updated)

func _unhandled_input(event: InputEvent) -> void:
	if event is InputEventMouseMotion and Input.mouse_mode == Input.MOUSE_MODE_CAPTURED:
		rotate_y(-event.relative.x * GameState.mouse_sensitivity * 0.01)
		pitch -= event.relative.y * GameState.mouse_sensitivity * 0.01
		pitch = clamp(pitch, deg_to_rad(-80), deg_to_rad(80))
		camera.rotation.x = pitch

func _physics_process(delta: float) -> void:
	if Input.is_key_pressed(KEY_LEFT):  rotate_y(LOOK_SPEED * delta)
	if Input.is_key_pressed(KEY_RIGHT): rotate_y(-LOOK_SPEED * delta)
	if Input.is_key_pressed(KEY_UP):    pitch += LOOK_SPEED * delta
	if Input.is_key_pressed(KEY_DOWN):  pitch -= LOOK_SPEED * delta
	pitch = clamp(pitch, deg_to_rad(-80), deg_to_rad(80))
	camera.rotation.x = pitch

	var fwd := false
	var back := false
	var left := false
	var right := false
	var shoot_held := false

	if alive:
		fwd = Input.is_action_pressed("move_forward")
		back = Input.is_action_pressed("move_back")
		left = Input.is_action_pressed("move_left")
		right = Input.is_action_pressed("move_right")
		shoot_held = Input.is_action_pressed("shoot")

		var f: Vector3 = -global_transform.basis.z
		var r: Vector3 = global_transform.basis.x
		var dir := Vector3.ZERO
		if fwd: dir += f
		if back: dir -= f
		if right: dir += r
		if left: dir -= r
		if dir.length() > 0.0:
			dir = dir.normalized()

		velocity.x = dir.x * PLAYER_SPEED
		velocity.z = dir.z * PLAYER_SPEED

		if is_on_floor():
			velocity.y = -0.1
			if Input.is_action_just_pressed("jump"):
				velocity.y = JUMP_VELOCITY
		else:
			velocity.y -= GRAVITY * delta

		move_and_slide()
	else:
		velocity.x = 0.0
		velocity.z = 0.0
		if not is_on_floor():
			velocity.y -= GRAVITY * delta
		else:
			velocity.y = 0.0
		move_and_slide()

	_send_accum += delta
	if _send_accum >= 1.0 / INPUT_SEND_HZ:
		_send_accum = 0.0
		var f3: Vector3 = -global_transform.basis.z
		var server_angle := atan2(f3.z, f3.x)
		Network.send_input(fwd, back, left, right, server_angle, shoot_held, pitch)

func _on_state_updated() -> void:
	if not Network.players.has(Network.my_id):
		return
	var me: Dictionary = Network.players[Network.my_id]
	alive = me["alive"]
	health = me["health"]
	ammo = me["ammo"]

	var target := LevelBuilderScript.map_to_world(me["x"], me["y"], global_position.y)
	global_position = global_position.lerp(target, RECONCILE_LERP)
