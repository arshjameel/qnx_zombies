extends CanvasLayer
class_name HUD

var _health_label: Label
var _ammo_label: Label
var _wave_label: Label
var _feed_label: RichTextLabel
var _feed_lines: Array = []  

var _health_bar: ProgressBar
var _health_bar_label: Label
var _ammo_label_bottom: Label
var _zombies_label: Label

const FEED_TTL := 5.0
const FEED_MAX_LINES := 4

func _ready() -> void:
	layer = 5

	var crosshair_wrap := CenterContainer.new()
	crosshair_wrap.set_anchors_preset(Control.PRESET_FULL_RECT)
	crosshair_wrap.mouse_filter = Control.MOUSE_FILTER_IGNORE
	add_child(crosshair_wrap)
	var crosshair := Label.new()
	crosshair.text = "+"
	crosshair.add_theme_font_size_override("font_size", 28)
	crosshair_wrap.add_child(crosshair)

	_health_label = _make_label(24, 16, 22)

	_ammo_label = _make_label(24, 48, 22)

	_zombies_label = _make_label(24, 16, 22)
	add_child(_zombies_label)

	_wave_label = Label.new()
	_wave_label.add_theme_font_size_override("font_size", 26)
	_wave_label.set_anchors_preset(Control.PRESET_CENTER_TOP)
	_wave_label.position = Vector2(-50, 16)
	_wave_label.size = Vector2(150, 40)
	add_child(_wave_label)

	_feed_label = RichTextLabel.new()
	_feed_label.bbcode_enabled = true
	_feed_label.fit_content = true
	_feed_label.scroll_active = false
	_feed_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
	_feed_label.set_anchors_preset(Control.PRESET_TOP_RIGHT)
	_feed_label.position = Vector2(-440, 16)
	_feed_label.size = Vector2(420, 140)
	_feed_label.add_theme_font_size_override("normal_font_size", 18)
	add_child(_feed_label)

	_health_bar = ProgressBar.new()
	_health_bar.min_value = 0
	_health_bar.max_value = 100
	_health_bar.value = 100
	_health_bar.show_percentage = false
	_health_bar.size = Vector2(220, 34)
	_health_bar.set_anchors_preset(Control.PRESET_BOTTOM_LEFT)
	_health_bar.position = Vector2(20, -48)
	add_child(_health_bar)

	_health_bar_label = Label.new()
	_health_bar_label.add_theme_font_size_override("font_size", 20)
	_health_bar_label.size = Vector2(220, 34)
	_health_bar_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	_health_bar_label.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	_health_bar_label.set_anchors_preset(Control.PRESET_BOTTOM_LEFT)
	_health_bar_label.position = Vector2(20, -48)
	add_child(_health_bar_label)

	_ammo_label_bottom = Label.new()
	_ammo_label_bottom.add_theme_font_size_override("font_size", 24)
	_ammo_label_bottom.size = Vector2(150, 28)
	_ammo_label_bottom.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
	_ammo_label_bottom.set_anchors_preset(Control.PRESET_BOTTOM_RIGHT)
	_ammo_label_bottom.position = Vector2(-170, -48)
	add_child(_ammo_label_bottom)

	Network.hit_event.connect(_on_hit_event)

func _make_label(x: float, y: float, font_size: int) -> Label:
	var l := Label.new()
	l.position = Vector2(x, y)
	l.add_theme_font_size_override("font_size", font_size)
	return l

func _process(delta: float) -> void:
	if Network.players.has(Network.my_id):
		var me: Dictionary = Network.players[Network.my_id]
		_health_label.text = "HP: %d" % int(me["health"])
		_health_label.modulate = Color.RED if me["health"] <= 25 else (Color.ORANGE if me["health"] <= 50 else Color.WHITE)
		_ammo_label.text = "AMMO: %d" % int(me["ammo"])

		var hp := int(me["health"])
		_health_bar.value = hp
		_health_bar_label.text = "%d/100" % hp
		_health_bar.modulate = Color.RED if hp <= 25 else (Color.ORANGE if hp <= 50 else Color.GREEN)

		_ammo_label_bottom.text = "AMMO: %d/60" % int(me["ammo"])

	_wave_label.text = "WAVE %d" % Network.wave
	_zombies_label.text = "ZOMBIES LEFT: %d" % Network.zombies.size()

	var changed := false
	for line in _feed_lines:
		line["ttl"] -= delta
	var before := _feed_lines.size()
	_feed_lines = _feed_lines.filter(func(l): return l["ttl"] > 0.0)
	if _feed_lines.size() != before:
		changed = true
	if changed:
		_rebuild_feed()

func _on_hit_event(victim_id: int, victim_type: int, attacker_id: int, _damage: int, headshot: bool) -> void:
	var text := ""
	if victim_type == Network.ENTITY_ZOMBIE:
		if headshot:
			text = "[color=orange][b][HEADSHOT][/b] Player %d hit zombie %d[/color]" % [attacker_id, victim_id]
		else:
			text = "[color=lightgreen]Player %d hit zombie %d[/color]" % [attacker_id, victim_id]
	else:
		if attacker_id == Network.ATTACKER_ZOMBIE:
			text = "[color=red]Zombie mauled Player %d[/color]" % victim_id
		else:
			text = "[color=yellow]Player %d hit Player %d[/color]" % [attacker_id, victim_id]
	_feed_lines.append({"text": text, "ttl": FEED_TTL})
	if _feed_lines.size() > FEED_MAX_LINES:
		_feed_lines.pop_front()
	_rebuild_feed()

func _rebuild_feed() -> void:
	var out := ""
	for line in _feed_lines:
		out += line["text"] + "\n"
	_feed_label.text = out
