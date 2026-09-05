extends CanvasLayer

signal quit_to_menu_pressed
signal quit_game_pressed

func _ready() -> void:
	layer = 10   
	Input.mouse_mode = Input.MOUSE_MODE_VISIBLE  

	var bg := ColorRect.new()
	bg.color = Color(0.0, 0.0, 0.0, 0.6)
	bg.set_anchors_preset(Control.PRESET_FULL_RECT)
	add_child(bg)

	var wrap := CenterContainer.new()
	wrap.set_anchors_preset(Control.PRESET_FULL_RECT)
	add_child(wrap)

	var panel := VBoxContainer.new()
	panel.custom_minimum_size = Vector2(360, 0)
	panel.add_theme_constant_override("separation", 20)
	wrap.add_child(panel)

	var label := Label.new()
	label.text = "Thank you for playing QNX Zombies"
	label.add_theme_font_size_override("font_size", 26)
	label.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	label.autowrap_mode = TextServer.AUTOWRAP_WORD
	label.custom_minimum_size = Vector2(360, 0)
	panel.add_child(label)

	panel.add_child(_make_button("Quit to Menu", func(): quit_to_menu_pressed.emit()))
	panel.add_child(_make_button("Quit Game", func(): quit_game_pressed.emit()))

func _make_button(text: String, on_press: Callable) -> Button:
	var b := Button.new()
	b.text = text
	b.custom_minimum_size = Vector2(280, 48)
	b.add_theme_font_size_override("font_size", 20)
	b.pressed.connect(on_press)
	return b
