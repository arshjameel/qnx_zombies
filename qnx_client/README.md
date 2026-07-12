# qnx_client -- native QNX Screen/EGL/GLES2 client

QNX-only. Does not compile on Linux -- no `libscreen`/`libEGL`/
`libGLESv2` there. That's the point of this redesign, not a bug.

`godot_client/` and `src/` (the server) are both untouched by this.
This client is a third, independent way to play: QNX becomes the
renderer as well as the server, using QNX's own native APIs instead
of Godot.

## Status: Procedural brick pattern on walls

Walls (and any other vertical-ish level surface) now get a real
procedural brick pattern instead of a single flat color, inspired by
QNX's own `gles2-maze` sample -- same core idea (`gl_FragColor = vcolor
* pattern`), but the pattern is generated from math in the fragment
shader rather than a sampled texture image, keeping this renderer's
"zero asset pipeline" approach intact (no `.tga`, no embedded texture
header, nothing to load at startup).

**How walls get distinguished from floor/ceiling/platforms in one draw
call:** every vertex now carries a real face normal (previously
`GeoVertex` had no normal at all -- purely flat per-vertex color). The
level fragment shader uses `1.0 - abs(normal.y)` as a "wallness"
factor: vertical wall faces (normal mostly horizontal) get the full
brick pattern, horizontal floor/ceiling/platform faces (normal mostly
vertical) stay flat, all from the same `level_vbo` draw call with no
per-tile flag needed.

**Two shader programs, not one:** this project reuses a single flat-
color shader for the 3D level, entities (player/zombie/pickup boxes),
*and* the 2D HUD/menu overlay -- adding the brick effect to that one
shader would have made zombies and HUD digits look like brick walls
too. Added a second program (`level_prog`) used only for the
`level_vbo` draw call; entities and HUD stay on the original simple
program, unchanged.

**Verified with a real GLSL compiler**, not just "it compiled in my
stub headers": installed `glslang-tools` and validated the actual
extracted shader source (not the escaped C string, the real GLSL) --
zero errors compiling each stage, zero errors linking each
vertex/fragment pair. Also verified the new face-normal math
numerically: every vertex's normal is unit length, wall faces get
exactly axis-aligned normals, and rotated ramp faces get correctly
tilted (non-axis-aligned) normals that stay unit length and pair up
antiparallel on opposite faces.

### Files changed this round

- `src/level_geo.h` -- `GeoVertex` gained `nx,ny,nz`; `vlist_push()`
  and `push_box()` signatures updated.
- `src/level_geo.c` -- `emit_box_faces()` now takes a per-face normal
  table; `push_box()` uses the fixed axis-aligned table;
  `push_box_transformed()` rotates it by the same world transform
  used for vertices (via a transform-point-minus-origin trick to
  strip out translation); `push_quad_y()` takes a normal_y parameter.
- `src/hud_render.c` -- `hud_push_quad()`'s `vlist_push()` calls
  updated with a dummy normal (unused by the HUD's shader program).
- `src/main.c` -- added `LEVEL_VERTEX_SHADER_SRC`/
  `LEVEL_FRAGMENT_SHADER_SRC` with the procedural brick pattern;
  `build_shader_program()` generalized to take shader sources as
  parameters; added `draw_vertex_list_lit()` for the 3-attribute
  level program; render loop now switches programs around the
  `level_vbo` draw call specifically.

**ESC now returns to the menu instead of quitting**, and you can
replay/restart from there. This needed a real distinction that didn't
exist before: `g_running` (the whole app's lifecycle) is now separate
from a new `g_net_thread_running` (just this play session's network
thread). ESC while playing stops the network thread, clears the
shared entity snapshot (so a stale zombie doesn't flash on screen for
a frame), and returns to `APP_STATE_MENU` -- selecting Play again
starts a fresh thread and resets the camera/input state cleanly. ESC
while already in the menu still quits the app, unchanged.

**Ramp/platform "floating" fixed -- both symptoms traced back to one
function, `level_height_for_tile()`:**
- Ramp tiles were returning a flat, single guessed height for any
  tile in the ramp, instead of the actual gradually-increasing height
  each physical tile represents. Fixed by extracting the chain-finding
  logic (previously only used to build the ramp's visual mesh) into a
  shared helper, `find_ramp_chain()`, so the camera's height query and
  the mesh generator now compute from the exact same source of truth.
  Verified numerically against the real map, not just reasoned about:
  the three ramp tiles at row 10 now report `0.333`, `1.0`, `1.667` --
  a real gradual climb -- instead of the old flat `1.0` for all three.
- Platform tiles were purely footprint-based -- standing anywhere
  under an elevated platform triggered the same "snap to platform
  height" as actually being on top of it, since the lookup only knew
  your `(x, y)`, not whether you'd climbed up. Fixed with an elevation
  gate (`ground_height_for_camera()`): a platform tile only counts as
  solid ground if the camera is already substantially elevated (i.e.
  arrived via the ramp); otherwise it's correctly treated as a ceiling
  above you, and the floor stays at 0.

### Files changed this round

- `src/level_geo.h` -- `PLATFORM_HEIGHT` promoted to a real shared
  constant (`LEVEL_PLATFORM_HEIGHT`) instead of being duplicated with
  a coupling comment, since `level_geo.c` and `main.c` are both C
  compiled together -- no cross-language reason to duplicate it here.
- `src/level_geo.c` -- extracted `find_ramp_chain()` from
  `build_ramp_chain()`; added `level_exact_ramp_height()`; updated
  `level_height_for_tile()` to use it.
- `src/main.c` -- added `g_net_thread_running`, `g_app_state`,
  `g_return_to_menu_requested`; state-aware ESC handling; the
  return-to-menu transition in the main loop; `PLATFORM_ELEVATED_THRESHOLD`
  and `ground_height_for_camera()` for the platform gate.

Health and ammo now render as e.g. `100/100` or `60/60` instead of
just the current value -- reusing the digit "1" itself as the
separator (its shape, a single vertical bar, reads as a slash between
two numbers), rather than building a new dedicated separator glyph.
`hud_render.c` gained `hud_number_width()` so multiple numbers can be
chained left-to-right with correct spacing regardless of digit count
(e.g. "9/60" needs less room than "60/60"). Ammo is right-aligned
using that measured width, so its right edge stays fixed as the
number of digits changes rather than shifting around.

`PLAYER_MAX_HEALTH`/`PLAYER_MAX_AMMO` (100/60) are defined in
`main.c` with a coupling note -- the server never sends a "max" value
over the wire, only current, so these have to be kept in sync with
`src/server.c`'s initial health and `AMMO_MAX` by hand.

**A real design constraint worth being upfront about:** GLES2 has no
text rendering, and `hud_render.c` was deliberately built around
7-segment numbers for exactly that reason. There's no way to spell
"SOLO" or "SETTINGS" as actual words anywhere in this client. So the
menu is numbered (1/2/3), not word-based: each option is a distinct
color plus its number, and the currently-selected option is shown at
full brightness while the other two are dimmed, rather than a
separate highlight/border box.

- **Play Solo** (green, 1) / **Play Coop** (blue, 2) / **Quit** (red,
  3). All three boxes are horizontally centered on the actual
  detected window width (not a hardcoded resolution), stacked
  vertically.
- Navigation reuses the same arrow keys used for look in-game, and E
  (the same fallback shoot key) to confirm -- safe to double up since
  menu vs. gameplay input is read by completely different code
  depending on which state the app is in.
- **The network thread does not start until Play Solo or Play Coop is
  confirmed.** Previously it connected immediately on launch; now
  connection only happens once you've actually chosen a mode, and
  that choice is what's sent as the `PktConnect` mode byte (this
  client was previously hardcoding `CONNECT_MODE_COOP` regardless of
  intent -- now it's real). Quit exits before ever opening a
  connection.
- **Settings and Credits from the Godot client are not here.**
  Settings would need either text input (server IP as a string) --
  not feasible without a virtual keyboard or similar -- or a fully
  custom numeric-only UI (e.g. editing an IP as 4 separate byte
  values). Credits has no functional value without any text
  rendering. Both were left out rather than shipped half-working; say
  the word if you want the numeric-IP-editing approach for Settings
  specifically, it's a real option, just scoped out of this round.

### Files changed this round

- `src/hud_render.h` / `src/hud_render.c` -- exposed the internal
  quad-drawing helper as `hud_push_quad()` so the menu can build
  custom shapes (selection boxes) reusing the same tested
  pixel-to-NDC conversion, instead of duplicating it.
- `src/main.c` -- added `APP_STATE_MENU`/`APP_STATE_PLAYING`, menu
  navigation state (edge-triggered, reusing existing keys),
  `build_menu_geometry()`, and restructured `main()`'s loop to render
  the menu and defer network-thread startup until Play is confirmed.

New this round:

- **Jump (Space bar).** Only while grounded; real gravity arc while
  airborne, using the same `GRAVITY`/`JUMP_VELOCITY` values as the
  Godot client's `Player.gd` for a consistent feel between the two
  clients. Purely local -- the server has no concept of player
  height at all, so jump never touches the network, same reasoning
  as the ramp/platform climbing. There's no physics engine here to
  ask "am I on the floor" the way Godot's `CharacterBody3D` can, so
  grounded/airborne is tracked explicitly (`Camera.jumping`), and the
  jump itself is rising-edge detected so holding Space doesn't
  re-jump every frame.
- **Ammo/health pickups** now render as small colored boxes
  (amber/brass for ammo, red for health, matching `Pickup.gd`),
  hidden while on cooldown. This was purely a client-side gap --
  `src/server.c` already fully implements pickup logic (position,
  cooldown, applying the effect when you walk over one); this client
  just wasn't parsing or drawing `PktState`'s pickup data before now.
  No server changes needed.

### Files changed this round

- `src/main.c` -- added `vel_y`/`jumping` to `Camera`; added
  `g_key_space`/`g_jump_requested` (edge-triggered) and the
  gravity/jump branch in `update_camera()`; added `RenderPickup`,
  the shared-state pickup array, parsing in the network thread, and
  rendering in `build_entity_geometry()`.

**The real bug behind both "can't kill zombies" and "WASD feels
random":** one root cause, not two. This client's own `cam->yaw` is
just an internal angle used to build the forward/right vectors for
rendering and movement -- it was being sent to the server as-is for
`look_angle`, but `src/server.c` computes its own facing direction as
`(cosf(angle), sinf(angle))`, a different convention. That mismatch
meant the server computed a rotated "forward" relative to what was
actually on screen: your shots (aimed using the server's angle) missed
a target that visually looked centered, and `reconcile_camera()` (the
previous fix) was pulling your camera toward a server position that
had been moving in that same rotated direction relative to your actual
WASD input -- which is exactly what "press W, go left instead"
feels like.

**Fix:** derive the angle actually sent to the server from the
already-computed forward vector via `atan2f()`, instead of sending
`cam->yaw` directly -- the same technique used in the Godot client's
`Player.gd` for the identical reason (self-consistent by construction,
rather than hand-deriving the exact offset between two conventions,
which is an easy way to get a sign or phase wrong -- as this was).
Verified numerically this time, not just reasoned about: tested 5
different yaw values through the conversion and confirmed the
resulting server-side forward vector matches the client's actual
forward vector in every case, not just the one I checked by hand
originally.

### Files changed this round

- `src/main.c` -- `update_camera()`'s `g_cam_yaw` assignment now uses
  `atan2f(fz, fx)` (the forward vector already computed earlier in
  the same function) instead of the raw `cam->yaw`.

Three issues reported after testing the previous round, all fixed:

1. **Enter didn't work as fallback shoot.** Turned out Enter doesn't
   report as plain ASCII `'\r'`/`'\n'` on this hardware -- swapped to
   **E** instead, checked as a plain ASCII letter exactly like
   W/A/S/D (which were already confirmed working). Left mouse button
   is unchanged.
2. **Zombie wouldn't chase, shots didn't land, took damage from
   nowhere.** These turned out to be one root cause, not three: the
   camera's rendered position was never reconciled against the
   server's authoritative position. Since zombie AI, hit detection,
   and melee damage are all computed server-side using the server's
   own belief of where you are -- not wherever this camera visually
   renders -- the two positions could silently drift apart with
   nothing pulling them back together. The zombie was correctly
   chasing/hitting a position you couldn't see. Fixed by
   `reconcile_camera()`: every frame, the camera's (x, z) is pulled
   toward the server's latest reported position for this player (not
   y/height, which stays fully local, same as the Godot client's
   approach). This is the same fundamental idea as `Player.gd`'s
   reconciliation on the Godot client, just implemented from scratch
   here since there's no equivalent system to lean on in raw C.

### Files changed this round

- `src/main.c` -- renamed the fallback shoot key from Enter to E;
  added `my_x`/`my_y`/`has_my_state` to the shared game state
  (extracted from our own entry in each `PktState`); added
  `reconcile_camera()`, called every frame right after
  `update_camera()`.

Everything from Stage 2a's fixes (resolution auto-detection, arrow-key
look) is unchanged. New this round:

- **Real network input.** The heartbeat placeholder is gone. The
  network thread sends actual held WASD state, the camera's real
  yaw/pitch, and shoot state every tick -- `src/server.c` now
  genuinely simulates this player (movement, shooting, zombie
  targeting all work), not just keeping the connection alive.
- **Shooting: left mouse button OR Enter.** Enter is checked via the
  raw ASCII value (`'\r'`), not a guessed `KEYCODE_ENTER` -- Enter is a
  standard ASCII control character, so there was no symbolic constant
  to look up at all. The left mouse button is read via
  `SCREEN_PROPERTY_BUTTONS` and `SCREEN_LEFT_MOUSE_BUTTON` -- this is
  the least-confirmed name in this codebase; if it fails to compile,
  check the actual button-type enum in your target's `screen.h`.
- **Zombies and other players render** as flat-colored boxes (green
  zombies, yellow teammates -- matching the Godot client's colors),
  rebuilt into a dynamic vertex buffer every frame from a
  mutex-protected snapshot of the latest `PktState`. This is the one
  place in this file with a real `pthread_mutex_t` -- everywhere else
  still uses plain `volatile` scalars, matching the rest of the
  project's approach, but a whole array of entities is a much likelier
  place for a torn read to visibly glitch.
- **HUD**: health, ammo, wave, and zombies-remaining as 7-segment
  digit numbers, plus a crosshair. GLES2 has no text rendering and
  pulling in a font library was out of scope, so `hud_render.c`
  implements the standard (non-QNX-specific) hardware 7-segment
  encoding directly as flat 2D quads -- this part was actually
  compiled and run, not just reasoned about: fed the digits for "73"
  and confirmed exactly 8 quads came out (7 draws 3 segments, 3 draws
  5 -- matches the standard encoding table), and confirmed every
  emitted vertex landed inside valid NDC range.
- **Known limitation, unchanged from before**: the camera's rendered
  position is still fully local (Stage 2a's free camera + collision),
  not reconciled against the server's authoritative position. Real
  input is sent and the server does simulate you, but if local
  collision ever disagrees with the server (e.g. on respawn), the
  camera won't snap to match. Reasonable next step once this is
  confirmed working, not done here.

### Files changed/added this round

- `src/level_geo.h` / `src/level_geo.c` -- exposed `push_box()` and
  `vlist_push()` (were file-local) so entity rendering can reuse them
  instead of duplicating box-drawing code.
- `src/hud_render.h` / `src/hud_render.c` -- new. 7-segment digit
  renderer + crosshair, emits directly in NDC space.
- `src/main.c` -- substantially rewritten: real input wiring, the
  mutex-protected shared entity snapshot, per-frame entity/HUD vertex
  building, Enter/left-mouse-button shoot detection, arrow-key look
  and display-resolution detection carried over from the previous
  round.
- `Makefile` -- added `hud_render.c` to the build.

### What's still NOT here (next steps)

- Server reconciliation for the local camera (see limitation above)
- Kill feed, headshot color feedback, pickup rendering
- Menu / solo-vs-coop selection (this client currently always
  connects in coop mode, hardcoded)

## What's been genuinely verified this time, vs. still unverified

Unlike Stage 1, part of this stage *could* actually be compiled and
run, since `level_geo.c` and `mat4.h` only depend on standard C plus
this project's own `map.h`/`map.c` -- no QNX-specific headers. That
part was compiled and executed for real (on a regular Linux machine,
linked against the actual `src/map.c`) before being included here:

- The ramp-chain detection (ported from `LevelBuilder.gd`) correctly
  found the floor→ramp→ramp→ramp→platform chain at row 10 with no
  warnings, and generated 5484 vertices / 1828 triangles for the
  whole level with no crashes.
- `level_height_for_tile()` returns exactly what it should at every
  tile type (0 on floor, the ramp midpoint, full height on the
  platform) -- this is what the Stage 2a camera uses to climb.

**Still entirely unverified**, because it depends on QNX-specific
headers I have no way to compile against: the GLES2 shader
compilation/linking, the actual Screen+EGL render loop, and whether
the camera genuinely *feels* right when driven by real WASD/mouse
input on hardware. If the level fails to render or the shader doesn't
link, check `stderr` for `[gl] shader compile failed` /
`[gl] program link failed` messages first -- they're deliberately
verbose for exactly this reason.

## Known limitation: mouse look sticks at screen edges

Mouse-look is computed by diffing consecutive absolute
`SCREEN_PROPERTY_POSITION` reads, not a raw relative-delta property --
I could not find clear QNX documentation for a pointer-recentering/
warp API to solve the classic "can't turn further once the cursor
hits the window edge" problem a real FPS needs. Worth flagging if it's
as annoying in practice as it sounds once you test it; solving it
properly is a good candidate for the next research pass.

## Building and testing

```bash
source ~/qnx800/qnxsdp-env.sh
cd qnx_client
make
make run
```

Same `qnxpi` ssh-alias-based deploy/run flow as before. You'll land in
the numbered menu first -- Up/Down to select, E to confirm. Once in
game: WASD to move, mouse or arrow keys to look, Space to jump, left
mouse button or E to shoot, ESC to return to the menu (ESC again from
there quits). Watch stdout for `[level] Built N
vertices` at startup -- if that number looks reasonable (should be a
few thousand for this map) but nothing renders, the problem is in the
GL/EGL setup, not the geometry.

## RTOS scheduling

Unchanged from Stage 1: render (main thread) and networking (a second
thread) run under explicit QNX real-time scheduling via
`pthread_setschedparam()` -- `SCHED_FIFO` at `sched_get_priority_max`
for render, the FIFO midpoint for networking. Both print at startup.

