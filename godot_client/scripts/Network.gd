extends Node

# ---- protocol constants (mirror net.h) ----
const NET_MAX_PLAYERS := 8
const MAX_ZOMBIES := 16

const PKT_CONNECT    := 0x01
const PKT_ACCEPT     := 0x02
const PKT_INPUT      := 0x03
const PKT_STATE      := 0x04
const PKT_HIT        := 0x06
const PKT_DISCONNECT := 0x07

const ENTITY_PLAYER   := 0
const ENTITY_ZOMBIE    := 1
const ATTACKER_ZOMBIE := 0xFF

const HEADER_SIZE       := 6    # u8 + u8 + u32
const ACCEPT_SIZE       := HEADER_SIZE + 1 + 4 + 4 + 4          # 19
const INPUT_SIZE        := HEADER_SIZE + 1 + 1 + 1 + 1 + 4 + 1  # 15
const PLAYER_STATE_SIZE := 1 + 1 + 4 + 4 + 4 + 1 + 1            # 16
const ZOMBIE_STATE_SIZE := 1 + 1 + 4 + 4 + 1                    # 11
const HIT_SIZE           := HEADER_SIZE + 1 + 1 + 1 + 1          # 10

signal connected(my_id: int, spawn_x: float, spawn_y: float, spawn_angle: float)
signal state_updated()
signal hit_event(victim_id: int, victim_type: int, attacker_id: int, damage: int)
signal disconnected_from_server()

var my_id: int = -1
var is_connected: bool = false

# Latest authoritative snapshot, keyed by id. Populated from PKT_STATE.
var players: Dictionary = {}   # id -> {alive, x, y, angle, health, ammo}
var zombies: Dictionary = {}   # id -> {alive, x, y, health}
var wave: int = 0

var _udp: PacketPeerUDP = PacketPeerUDP.new()
var _local_tick: int = 0
var _connect_retry_timer: float = 0.0
const CONNECT_RETRY_INTERVAL := 0.5
const CONNECT_TIMEOUT := 8.0
var _connect_elapsed: float = 0.0
var _connecting: bool = false

func _process(delta: float) -> void:
	_poll_incoming()
	if _connecting and not is_connected:
		_connect_retry_timer -= delta
		_connect_elapsed += delta
		if _connect_elapsed > CONNECT_TIMEOUT:
			_connecting = false
			push_warning("Network: connection to server timed out")
			return
		if _connect_retry_timer <= 0.0:
			_send_connect()
			_connect_retry_timer = CONNECT_RETRY_INTERVAL

## Call from the menu when the player picks Solo or Coop.
func begin_connect(ip: String, port: int) -> void:
	reset()
	var err := _udp.connect_to_host(ip, port)
	if err != OK:
		push_warning("Network: connect_to_host failed: %s" % err)
		return
	_connecting = true
	_connect_elapsed = 0.0
	_connect_retry_timer = 0.0  # send immediately on next _process

func reset() -> void:
	my_id = -1
	is_connected = false
	players.clear()
	zombies.clear()
	wave = 0
	_connecting = false

func send_disconnect() -> void:
	if not is_connected:
		return
	var pba := StreamPeerBuffer.new()
	_write_header(pba, PKT_DISCONNECT, my_id)
	_udp.put_packet(pba.data_array)
	is_connected = false

## Call once per physics frame while playing.
func send_input(forward: bool, back: bool, left: bool, right: bool,
		look_angle: float, shoot: bool) -> void:
	if not is_connected:
		return
	var pba := StreamPeerBuffer.new()
	_write_header(pba, PKT_INPUT, my_id)
	pba.put_u8(1 if forward else 0)
	pba.put_u8(1 if back else 0)
	pba.put_u8(1 if left else 0)
	pba.put_u8(1 if right else 0)
	pba.put_float(look_angle)
	pba.put_u8(1 if shoot else 0)
	_udp.put_packet(pba.data_array)

func _send_connect() -> void:
	var pba := StreamPeerBuffer.new()
	_write_header(pba, PKT_CONNECT, 0)
	_udp.put_packet(pba.data_array)

func _write_header(pba: StreamPeerBuffer, type: int, player_id: int) -> void:
	pba.big_endian = false
	pba.put_u8(type)
	pba.put_u8(player_id)
	pba.put_u32(_local_tick)
	_local_tick += 1

func _poll_incoming() -> void:
	while _udp.get_available_packet_count() > 0:
		var bytes: PackedByteArray = _udp.get_packet()
		if bytes.size() < HEADER_SIZE:
			continue
		var pba := StreamPeerBuffer.new()
		pba.big_endian = false
		pba.data_array = bytes
		var type := pba.get_u8()
		var _hdr_player_id := pba.get_u8()
		var _hdr_tick := pba.get_u32()

		match type:
			PKT_ACCEPT:
				if bytes.size() < ACCEPT_SIZE: continue
				var assigned_id := pba.get_u8()
				var sx := pba.get_float()
				var sy := pba.get_float()
				var sa := pba.get_float()
				my_id = assigned_id
				is_connected = true
				_connecting = false
				connected.emit(my_id, sx, sy, sa)

			PKT_STATE:
				if bytes.size() < HEADER_SIZE + 1: continue
				var player_count := pba.get_u8()
				var new_players := {}
				for i in range(NET_MAX_PLAYERS):
					var pid := pba.get_u8()
					var alive := pba.get_u8()
					var x := pba.get_float()
					var y := pba.get_float()
					var angle := pba.get_float()
					var health := pba.get_u8()
					var ammo := pba.get_u8()
					if i < player_count:
						new_players[pid] = {
							"alive": alive != 0, "x": x, "y": y,
							"angle": angle, "health": health, "ammo": ammo,
						}
				var zombie_count := pba.get_u8()
				var new_zombies := {}
				for i in range(MAX_ZOMBIES):
					var zid := pba.get_u8()
					var zalive := pba.get_u8()
					var zx := pba.get_float()
					var zy := pba.get_float()
					var zhealth := pba.get_u8()
					if i < zombie_count:
						new_zombies[zid] = {
							"alive": zalive != 0, "x": zx, "y": zy, "health": zhealth,
						}
				wave = pba.get_u8()
				players = new_players
				zombies = new_zombies
				state_updated.emit()

			PKT_HIT:
				if bytes.size() < HIT_SIZE: continue
				var victim_id := pba.get_u8()
				var victim_type := pba.get_u8()
				var attacker_id := pba.get_u8()
				var damage := pba.get_u8()
				hit_event.emit(victim_id, victim_type, attacker_id, damage)

			_:
				pass  # unknown packet type, ignore
