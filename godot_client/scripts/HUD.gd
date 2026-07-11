extends CanvasLayer
class_name HUD

var _health_label: Label
var _ammo_label: Label
var _wave_label: Label
var _feed_label: RichTextLabel
var _feed_lines: Array = []   # [{text, ttl}]

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
	add_child(_health_label)

	_ammo_label = _make_label(24, 48, 22)
	add_child(_ammo_label)

	_wave_label = Label.new()
	_wave_label.add_theme_font_size_override("font_size", 26)
	_wave_label.set_anchors_preset(Control.PRESET_TOP_RIGHT)
	_wave_label.position = Vector2(-160, 16)
	_wave_label.size = Vector2(150, 40)
	add_child(_wave_label)

	_feed_label = RichTextLabel.new()
	_feed_label.bbcode_enabled = true
	_feed_label.fit_content = true
	_feed_label.scroll_active = false
	_feed_label.position = Vector2(16, 90)
	_feed_label.size = Vector2(420, 140)
	_feed_label.add_theme_font_size_override("normal_font_size", 18)
	add_child(_feed_label)

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
	_wave_label.text = "WAVE %d" % Network.wave

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
			text = "[color=orange][b]HEADSHOT[/b] -- Player %d dropped zombie %d[/color]" % [attacker_id, victim_id]
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
