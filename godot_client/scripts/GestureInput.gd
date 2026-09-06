extends Node
## Autoload: GestureInput
## Listens on a LOCAL-ONLY UDP socket for "SHOOT" packets sent by
## tools/gesture_shoot.py (Python + MediaPipe Hands, finger-gun pose
## detection). This is purely a local input source, exactly like
## Input.is_action_pressed("shoot") for the mouse -- it never touches
## Network.gd or the game server's protocol at all. Player.gd just
## OR's is_active() in alongside the regular shoot input.
##
## Why UDP instead of embedding MediaPipe in Godot: MediaPipe's mature
## APIs are Python/C++/JS, none of which are GDExtension-friendly
## without a heavy native build. A tiny local socket is effectively
## free (sub-ms loopback) and keeps the two runtimes fully decoupled --
## same reasoning as the QNX-side plan (camera/AI process talks over a
## local socket rather than trying to link MediaPipe into the engine).

const GESTURE_PORT := 5555

## How long to keep treating the gesture as "held" after the last
## packet arrives. The Python side sends one packet per processed
## camera frame (roughly 15-30/sec) while the pose holds, so this just
## needs to comfortably bridge the gap between frames.
const HOLD_TIMEOUT_MS := 200

## Toggle key -- press once to launch tools/gesture_shoot.py in the
## background (--headless, no preview window), press again to kill it.
## Optional input source: if you never press G, this whole feature
## just sits idle and mouse/keyboard shoot works exactly as before.
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

## True if a SHOOT packet arrived recently enough to still count as
## "held" -- mirrors Input.is_action_pressed()'s semantics so Player.gd
## can just OR the two together.
func is_active() -> bool:
	return _bound and (Time.get_ticks_msec() - _last_shoot_ms < HOLD_TIMEOUT_MS)

## Same idea as is_active(), for the two look-turn gestures. Left hand
## pointing left / right hand pointing right, per gesture_shoot.py.
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
	# tools/gesture_shoot.py lives one level above the Godot project
	# root (godot_client/), as a sibling of src/ -- see repo layout.
	var script_path := ProjectSettings.globalize_path("res://../tools/gesture_shoot.py")
	if not FileAccess.file_exists(script_path):
		push_warning("GestureInput: couldn't find gesture_shoot.py at %s -- check the path or run it manually" % script_path)
		return

	# "python3" first (Linux/WSL/macOS convention); falls back to
	# "python" below if that's what your Windows/venv setup uses instead.
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
	# Don't leave a stale "held" state for any gesture after killing it.
	_last_shoot_ms = -1000000
	_last_look_left_ms = -1000000
	_last_look_right_ms = -1000000
	_last_walk_forward_ms = -1000000
	print("GestureInput: camera gesture shooting OFF")

func _exit_tree() -> void:
	# Don't leave an orphaned webcam process running if the game quits
	# while the camera feature is still on.
	if _process_id > 0:
		OS.kill(_process_id)
