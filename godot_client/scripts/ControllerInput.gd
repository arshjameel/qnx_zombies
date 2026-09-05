extends Node

const CONTROLLER_PORT := 5556
const PACKET_LEN := 5

const STICK_CENTER := 127
const MOVE_DEADZONE := 40   
const LOOK_DEADZONE := 25   

const TIMEOUT_MS := 250

const BTN_JUMP := 0x01   
const BTN_SHOOT := 0x02  

var _udp := PacketPeerUDP.new()
var _bound: bool = false
var _last_packet_ms: int = -1000000

var _x1: int = STICK_CENTER
var _y1: int = STICK_CENTER
var _x2: int = STICK_CENTER
var _y2: int = STICK_CENTER
var _buttons: int = 0
var _prev_jump_pressed: bool = false

func _ready() -> void:
	var err := _udp.bind(CONTROLLER_PORT, "127.0.0.1")
	_bound = (err == OK)
	if not _bound:
		push_warning("ControllerInput: failed to bind 127.0.0.1:%d (err %d) -- Arduino controller disabled, keyboard/mouse still works" % [CONTROLLER_PORT, err])

func _process(_delta: float) -> void:
	if not _bound:
		return
	while _udp.get_available_packet_count() > 0:
		var bytes := _udp.get_packet()
		if bytes.size() != PACKET_LEN:
			continue
		_x1 = bytes[0]
		_y1 = bytes[1]
		_x2 = bytes[2]
		_y2 = bytes[3]
		_buttons = bytes[4]
		_last_packet_ms = Time.get_ticks_msec()

func _is_fresh() -> bool:
	return _bound and (Time.get_ticks_msec() - _last_packet_ms < TIMEOUT_MS)

func _axis(raw: int, deadzone: int) -> float:
	if not _is_fresh():
		return 0.0
	var delta := raw - STICK_CENTER
	if abs(delta) < deadzone:
		return 0.0
	return clamp(float(delta) / float(STICK_CENTER), -1.0, 1.0)

func is_move_forward() -> bool:
	return _axis(_y1, MOVE_DEADZONE) > 0.0

func is_move_back() -> bool:
	return _axis(_y1, MOVE_DEADZONE) < 0.0

func is_move_left() -> bool:
	return _axis(_x1, MOVE_DEADZONE) < 0.0

func is_move_right() -> bool:
	return _axis(_x1, MOVE_DEADZONE) > 0.0

func get_look_vector() -> Vector2:
	return Vector2(_axis(_x2, LOOK_DEADZONE), -_axis(_y2, LOOK_DEADZONE))

func is_shoot_active() -> bool:
	return _is_fresh() and (_buttons & BTN_SHOOT) != 0

func is_jump_just_pressed() -> bool:
	var pressed := _is_fresh() and (_buttons & BTN_JUMP) != 0
	var just_pressed := pressed and not _prev_jump_pressed
	_prev_jump_pressed = pressed
	return just_pressed

func is_controller_connected() -> bool:
	return _is_fresh()
