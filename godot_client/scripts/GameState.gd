extends Node
## Autoload: GameState
## Holds settings and menu selections that need to survive scene changes.
## Nothing here is networked -- it's purely local client config.

# Default points at the QNX Pi. Change in Settings, or override here.
# NOTE: 169.254.x.x is a link-local (APIPA) address -- it only routes
# over a direct link (e.g. Pi connected straight to this laptop via
# Ethernet, no switch/router in between). If you go back to Wi-Fi with
# a normal DHCP/static LAN address, update this.
var server_ip: String = "169.254.121.132"
var server_port: int = 7777

var mouse_sensitivity: float = 0.15
var fov_degrees: float = 90.0
var master_volume: float = 1.0

# "solo" or "coop" -- flavor only. The server hosts a single shared
# world regardless; this just affects menu copy / HUD framing.
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
