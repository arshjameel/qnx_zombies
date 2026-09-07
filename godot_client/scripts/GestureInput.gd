extends Node

const GESTURE_PORT := 5555

const HOLD_TIMEOUT_MS := 200

const TOGGLE_KEY := KEY_G

var _udp := PacketPeerUDP.new()
var _last_shoot_ms: int = -1000000
var _last_look_left_ms: int = -1000000
var _last_look_right_ms: int = -1000000
var _last_walk_forward_ms: int = -1000000
var _bound: bool = false

var _process_id: int = -1
var _camera_active: bool = false

func _ready() -> void:
	var err := _udp.bind(GESTURE_PORT, "127.0.0.1")
	_bound = (err == OK)
	if not _bound:
		push_warning("GestureInput: failed to bind 127.0.0.1:%d (err %d) -- gesture shooting disabled, mouse/keyboard shoot still works" % [GESTURE_PORT, err])

func _process(_delta: float) -> void:
	if not _bound:
		return
	while _udp.get_available_packet_count() > 0:
		var bytes := _udp.get_packet()
		var msg := bytes.get_string_from_utf8()
		match msg:
			"SHOOT":
				_last_shoot_ms = Time.get_ticks_msec()
			"LOOK_LEFT":
				_last_look_left_ms = Time.get_ticks_msec()
			"LOOK_RIGHT":
				_last_look_right_ms = Time.get_ticks_msec()
			"WALK_FORWARD":
				_last_walk_forward_ms = Time.get_ticks_msec()

func _unhandled_input(event: InputEvent) -> void:
	if event is InputEventKey and event.pressed and not event.echo and event.keycode == TOGGLE_KEY:
		_toggle_camera()

func is_active() -> bool:
	return _bound and (Time.get_ticks_msec() - _last_shoot_ms < HOLD_TIMEOUT_MS)

func is_look_left_active() -> bool:
	return _bound and (Time.get_ticks_msec() - _last_look_left_ms < HOLD_TIMEOUT_MS)

func is_look_right_active() -> bool:
	return _bound and (Time.get_ticks_msec() - _last_look_right_ms < HOLD_TIMEOUT_MS)

func is_walk_forward_active() -> bool:
	return _bound and (Time.get_ticks_msec() - _last_walk_forward_ms < HOLD_TIMEOUT_MS)

func is_camera_active() -> bool:
	return _camera_active

func _toggle_camera() -> void:
	if _camera_active:
		_stop_camera()
	else:
		_start_camera()

func _start_camera() -> void:
	var script_path := ProjectSettings.globalize_path("res://../tools/gesture_shoot.py")
	if not FileAccess.file_exists(script_path):
		push_warning("GestureInput: couldn't find gesture_shoot.py at %s -- check the path or run it manually" % script_path)
		return

	_process_id = OS.create_process("python3", [script_path, "--headless"])
	if _process_id <= 0:
		_process_id = OS.create_process("python", [script_path, "--headless"])
	if _process_id <= 0:
		push_warning("GestureInput: failed to launch gesture_shoot.py -- is python3/python on PATH?")
		_process_id = -1
		return

	_camera_active = true
	print("GestureInput: camera gesture shooting ON (pid %d) -- press G again to stop" % _process_id)

func _stop_camera() -> void:
	if _process_id > 0:
		OS.kill(_process_id)
	_process_id = -1
	_camera_active = false
	_last_shoot_ms = -1000000
	_last_look_left_ms = -1000000
	_last_look_right_ms = -1000000
	_last_walk_forward_ms = -1000000
	print("GestureInput: camera gesture shooting OFF")

func _exit_tree() -> void:
	if _process_id > 0:
		OS.kill(_process_id)
