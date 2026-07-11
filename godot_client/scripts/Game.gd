extends Node3D

const PlayerScript       := preload("res://scripts/Player.gd")
const LevelBuilderScript := preload("res://scripts/LevelBuilder.gd")
const HUDScript          := preload("res://scripts/HUD.gd")
const RemotePlayerScript := preload("res://scripts/RemotePlayer.gd")
const ZombieScript       := preload("res://scripts/Zombie.gd")

var _player: CharacterBody3D = null
var _remote_players: Dictionary = {}   # id -> RemotePlayer node
var _zombies: Dictionary = {}          # id -> Zombie node
var _level: Node3D
var _entities_root: Node3D
var _hud: CanvasLayer

func _ready() -> void:
	var env := WorldEnvironment.new()
	var environment := Environment.new()
	environment.background_mode = Environment.BG_COLOR
	environment.background_color = Color(0.02, 0.02, 0.03)
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	environment.ambient_light_color = Color(0.35, 0.32, 0.30)
	environment.ambient_light_energy = 0.6
	env.environment = environment
	add_child(env)

	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-55, -30, 0)
	sun.light_energy = 0.7
	add_child(sun)

	_level = LevelBuilderScript.new()
	add_child(_level)

	_entities_root = Node3D.new()
	add_child(_entities_root)

	_hud = HUDScript.new()
	add_child(_hud)

	Network.connected.connect(_on_connected)
	Network.state_updated.connect(_on_state_updated)
	Network.begin_connect(GameState.server_ip, GameState.server_port)

func _on_connected(my_id: int, spawn_x: float, spawn_y: float, _spawn_angle: float) -> void:
	if _player != null:
		return
	_player = PlayerScript.new()
	add_child(_player)
	_player.global_position = LevelBuilderScript.map_to_world(spawn_x, spawn_y, 0.0)
	# Not bothering to convert spawn_angle into an exact initial yaw --
	# it's a cosmetic starting facing direction and self-corrects the
	# instant the player moves the mouse.
	print("Connected as player %d" % my_id)

func _on_state_updated() -> void:
	_sync_remote_players()
	_sync_zombies()

func _sync_remote_players() -> void:
	var seen := {}
	for id in Network.players.keys():
		if id == Network.my_id:
			continue
		seen[id] = true
		if not _remote_players.has(id):
			var rp := RemotePlayerScript.new()
			rp.player_id = id
			_entities_root.add_child(rp)
			_remote_players[id] = rp
		_remote_players[id].update_from_state(Network.players[id])

	for id in _remote_players.keys().duplicate():
		if not seen.has(id):
			_remote_players[id].queue_free()
			_remote_players.erase(id)

func _sync_zombies() -> void:
	var seen := {}
	for id in Network.zombies.keys():
		seen[id] = true
		if not _zombies.has(id):
			var z := ZombieScript.new()
			z.zombie_id = id
			_entities_root.add_child(z)
			_zombies[id] = z
		_zombies[id].update_from_state(Network.zombies[id])

	for id in _zombies.keys().duplicate():
		if not seen.has(id):
			_zombies[id].queue_free()
			_zombies.erase(id)

func _unhandled_input(event: InputEvent) -> void:
	if event.is_action_pressed("ui_cancel"):
		_quit_to_menu()

func _quit_to_menu() -> void:
	Network.send_disconnect()
	Input.mouse_mode = Input.MOUSE_MODE_VISIBLE
	get_tree().change_scene_to_file("res://scenes/MainMenu.tscn")
