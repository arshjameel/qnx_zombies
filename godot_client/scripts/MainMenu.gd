extends Control

var _root_wrap: CenterContainer
var _settings_wrap: CenterContainer
var _credits_wrap: CenterContainer

var _root_panel: VBoxContainer
var _settings_panel: VBoxContainer
var _credits_panel: VBoxContainer

var _ip_edit: LineEdit
var _sens_slider: HSlider
var _fov_slider: HSlider
var _volume_slider: HSlider

func _ready() -> void:
	set_anchors_preset(Control.PRESET_FULL_RECT)
	Input.mouse_mode = Input.MOUSE_MODE_VISIBLE

	var bg := ColorRect.new()
	bg.color = Color(0.05, 0.05, 0.07)
	bg.set_anchors_preset(Control.PRESET_FULL_RECT)
	add_child(bg)

	var title := Label.new()
	title.text = "QNX GAME"
	title.add_theme_font_size_override("font_size", 48)
	title.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	title.set_anchors_preset(Control.PRESET_TOP_WIDE)
	title.offset_top = 60
	title.offset_bottom = 120
	add_child(title)

	_build_main_panel()
	_build_settings_panel()
	_build_credits_panel()

	_show_panel(_root_wrap)

func _wrap_centered(content: Control) -> CenterContainer:
	var wrap := CenterContainer.new()
	wrap.set_anchors_preset(Control.PRESET_FULL_RECT)
	wrap.mouse_filter = Control.MOUSE_FILTER_IGNORE
	add_child(wrap)
	wrap.add_child(content)
	return wrap

func _build_main_panel() -> void:
	_root_panel = VBoxContainer.new()
	_root_panel.custom_minimum_size = Vector2(240, 0)
	_root_panel.add_theme_constant_override("separation", 14)
	_root_wrap = _wrap_centered(_root_panel)

	_root_panel.add_child(_make_button("Play Solo", func(): _play("solo")))
	_root_panel.add_child(_make_button("Play Coop", func(): _play("coop")))
	_root_panel.add_child(_make_button("Settings", func(): _show_panel(_settings_wrap)))
	_root_panel.add_child(_make_button("Credits", func(): _show_panel(_credits_wrap)))
	_root_panel.add_child(_make_button("Quit", func(): get_tree().quit()))

func _build_settings_panel() -> void:
	_settings_panel = VBoxContainer.new()
	_settings_panel.custom_minimum_size = Vector2(340, 0)
	_settings_panel.add_theme_constant_override("separation", 10)
	_settings_wrap = _wrap_centered(_settings_panel)

	_settings_panel.add_child(_make_label("SETTINGS", 26))

	_settings_panel.add_child(_make_label("Server IP (the QNX Pi)", 16))
	_ip_edit = LineEdit.new()
	_ip_edit.text = GameState.server_ip
	_ip_edit.custom_minimum_size = Vector2(300, 0)
	_settings_panel.add_child(_ip_edit)

	_settings_panel.add_child(_make_label("Mouse Sensitivity", 16))
	_sens_slider = _make_slider(0.02, 1.0, GameState.mouse_sensitivity)
	_settings_panel.add_child(_sens_slider)

	_settings_panel.add_child(_make_label("Field of View", 16))
	_fov_slider = _make_slider(60.0, 110.0, GameState.fov_degrees)
	_settings_panel.add_child(_fov_slider)

	_settings_panel.add_child(_make_label("Master Volume", 16))
	_volume_slider = _make_slider(0.0, 1.0, GameState.master_volume)
	_settings_panel.add_child(_volume_slider)

	_settings_panel.add_child(_make_button("Save & Back", func(): _save_settings()))

func _build_credits_panel() -> void:
	_credits_panel = VBoxContainer.new()
	_credits_panel.custom_minimum_size = Vector2(420, 0)
	_credits_panel.add_theme_constant_override("separation", 10)
	_credits_wrap = _wrap_centered(_credits_panel)

	_credits_panel.add_child(_make_label("CREDITS", 26))
	_credits_panel.add_child(_make_label("Built for a 15-hour hackathon.", 16))
	_credits_panel.add_child(_make_label("Server: C99, runs on QNX Neutrino 8.0 (Raspberry Pi 5).", 16))
	_credits_panel.add_child(_make_label("Client: Godot 4, cross-platform coop FPS.", 16))
	_credits_panel.add_child(_make_button("Back", func(): _show_panel(_root_wrap)))

func _make_button(text: String, on_press: Callable) -> Button:
	var b := Button.new()
	b.text = text
	b.custom_minimum_size = Vector2(240, 44)
	b.add_theme_font_size_override("font_size", 20)
	b.pressed.connect(on_press)
	return b

func _make_label(text: String, font_size: int) -> Label:
	var l := Label.new()
	l.text = text
	l.add_theme_font_size_override("font_size", font_size)
	l.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	l.autowrap_mode = TextServer.AUTOWRAP_WORD
	l.custom_minimum_size = Vector2(380, 0)
	return l

func _make_slider(min_v: float, max_v: float, value: float) -> HSlider:
	var s := HSlider.new()
	s.min_value = min_v
	s.max_value = max_v
	s.step = (max_v - min_v) / 100.0
	s.value = value
	s.custom_minimum_size = Vector2(320, 24)
	return s

func _show_panel(wrap: CenterContainer) -> void:
	_root_wrap.visible = (wrap == _root_wrap)
	_settings_wrap.visible = (wrap == _settings_wrap)
	_credits_wrap.visible = (wrap == _credits_wrap)

func _save_settings() -> void:
	GameState.server_ip = _ip_edit.text.strip_edges()
	GameState.mouse_sensitivity = _sens_slider.value
	GameState.fov_degrees = _fov_slider.value
	GameState.master_volume = _volume_slider.value
	GameState.save_settings()
	_show_panel(_root_wrap)

func _play(mode: String) -> void:
	GameState.mode = mode
	get_tree().change_scene_to_file("res://scenes/Game.tscn")
