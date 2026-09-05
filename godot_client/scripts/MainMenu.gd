extends Control

var _root_wrap: CenterContainer
var _settings_wrap: CenterContainer

var _root_panel: VBoxContainer
var _settings_panel: VBoxContainer

var _ip_edit: LineEdit
var _sens_slider: HSlider
var _fov_slider: HSlider

func _ready() -> void:
	set_anchors_preset(Control.PRESET_FULL_RECT)
	set_deferred("size", get_viewport_rect().size)
	Input.mouse_mode = Input.MOUSE_MODE_VISIBLE

	var bg := ColorRect.new()
	bg.color = Color(0, 0, 0)   
	bg.set_anchors_preset(Control.PRESET_FULL_RECT)
	add_child(bg)

	_build_main_panel()
	_build_settings_panel()

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
	_root_panel.custom_minimum_size = Vector2(260, 0)
	_root_panel.add_theme_constant_override("separation", 14)
	_root_wrap = _wrap_centered(_root_panel)

	var title := Label.new()
	title.text = "QNX ZOMBIES"
	title.add_theme_font_size_override("font_size", 48)
	title.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	_root_panel.add_child(title)

	var spacer := Control.new()
	spacer.custom_minimum_size = Vector2(0, 30)
	_root_panel.add_child(spacer)

	_root_panel.add_child(_make_colored_button("Play Solo", Color(0.25, 0.70, 0.30),
		func(): _play("solo")))
	_root_panel.add_child(_make_colored_button("Play Coop", Color(0.25, 0.45, 0.85),
		func(): _play("coop")))
	_root_panel.add_child(_make_colored_button("Settings", Color(0.55, 0.30, 0.65),
		func(): _show_panel(_settings_wrap)))
	_root_panel.add_child(_make_colored_button("Quit", Color(0.65, 0.20, 0.20),
		func(): get_tree().quit()))

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

	_settings_panel.add_child(_make_button("Save & Back", func(): _save_settings()))

func _make_button(text: String, on_press: Callable) -> Button:
	var b := Button.new()
	b.text = text
	b.custom_minimum_size = Vector2(240, 44)
	b.add_theme_font_size_override("font_size", 20)
	b.pressed.connect(on_press)
	return b

func _make_colored_button(text: String, color: Color, on_press: Callable) -> Button:
	var b := Button.new()
	b.text = text
	b.custom_minimum_size = Vector2(260, 48)
	b.add_theme_font_size_override("font_size", 20)

	var normal := StyleBoxFlat.new()
	normal.bg_color = color
	b.add_theme_stylebox_override("normal", normal)

	var hover := StyleBoxFlat.new()
	hover.bg_color = color.lightened(0.15)
	b.add_theme_stylebox_override("hover", hover)

	var pressed := StyleBoxFlat.new()
	pressed.bg_color = color.darkened(0.15)
	b.add_theme_stylebox_override("pressed", pressed)

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

func _save_settings() -> void:
	GameState.server_ip = _ip_edit.text.strip_edges()
	GameState.mouse_sensitivity = _sens_slider.value
	GameState.fov_degrees = _fov_slider.value
	GameState.save_settings()
	_show_panel(_root_wrap)

func _play(mode: String) -> void:
	GameState.mode = mode
	get_tree().change_scene_to_file("res://scenes/Game.tscn")
