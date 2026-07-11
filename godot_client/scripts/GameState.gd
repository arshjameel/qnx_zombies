extends Node

var server_ip: String = "169.254.121.132"
var server_port: int = 7777

var mouse_sensitivity: float = 0.15
var fov_degrees: float = 90.0
var master_volume: float = 1.0

# "solo" or "coop". currently wip
var mode: String = "solo"

const SETTINGS_PATH := "user://settings.cfg"

func _ready() -> void:
	load_settings()

func save_settings() -> void:
	var cfg := ConfigFile.new()
	cfg.set_value("client", "server_ip", server_ip)
	cfg.set_value("client", "server_port", server_port)
	cfg.set_value("client", "mouse_sensitivity", mouse_sensitivity)
	cfg.set_value("client", "fov_degrees", fov_degrees)
	cfg.set_value("client", "master_volume", master_volume)
	cfg.save(SETTINGS_PATH)

func load_settings() -> void:
	var cfg := ConfigFile.new()
	if cfg.load(SETTINGS_PATH) != OK:
		return
	server_ip          = cfg.get_value("client", "server_ip", server_ip)
	server_port        = cfg.get_value("client", "server_port", server_port)
	mouse_sensitivity  = cfg.get_value("client", "mouse_sensitivity", mouse_sensitivity)
	fov_degrees        = cfg.get_value("client", "fov_degrees", fov_degrees)
	master_volume      = cfg.get_value("client", "master_volume", master_volume)
