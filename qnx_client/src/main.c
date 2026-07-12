/*
 * qnx_client/src/main.c -- STAGE 2b + zombies/HUD.
 *
 * Builds on Stage 2a (validated on real hardware: level renders
 * correctly, WASD/mouse-look camera works). New this stage:
 *
 *   - REAL network input: the heartbeat placeholder from Stage 1/2a
 *     is gone. The network thread now sends the actual held WASD
 *     state, the camera's real yaw/pitch, and whether the player is
 *     shooting (left mouse button OR Enter -- see below) every tick.
 *   - The network thread now parses the FULL PktState (every player,
 *     every zombie, not just the counts printed before) into a
 *     mutex-protected shared struct the render thread reads once per
 *     frame. This is the one place in this file that needs real
 *     synchronization -- the scalar flags elsewhere (g_connected,
 *     g_key_w, etc.) stay plain `volatile`, matching the rest of this
 *     project's "good enough for a hackathon" approach to cross-
 *     thread scalars, but a whole snapshot of every entity is a much
 *     more likely place for a torn read to actually look wrong on
 *     screen, so it gets a real mutex.
 *   - Zombies and other players are rendered as flat-colored boxes
 *     (green for zombies, yellow for teammates), built fresh into a
 *     dynamic vertex buffer every frame from that shared snapshot --
 *     unlike the level, which is static geometry uploaded once.
 *   - A HUD: health/ammo/wave/zombies-left as 7-segment digit
 *     numbers (see hud_render.c for why -- no font rendering in
 *     GLES2, and pulling in a text-rendering library was out of
 *     scope), plus a crosshair. Drawn as a 2D overlay after the 3D
 *     scene, depth test disabled, using the same shader with an
 *     identity MVP (hud_render.c already emits NDC coordinates
 *     directly).
 *   - E as a fallback shoot button, alongside the left mouse button,
 *     for players using arrow-key-only look. Checked as a plain ASCII
 *     letter (same as W/A/S/D). Originally this used Enter, checked
 *     via raw ASCII '\r'/'\n' -- that didn't work in practice on real
 *     hardware (Enter apparently doesn't report as plain CR/LF the
 *     way this console/BSP delivers key events), so it was swapped
 *     for a plain letter key instead, which is already confirmed
 *     working via WASD.
 *   - Jump: Space bar, only while grounded, with real gravity while
 *     airborne (GRAVITY/JUMP_VELOCITY, matching the Godot client's
 *     values for a consistent feel). Purely local, like the Godot
 *     client's jump -- the server has no concept of player height at
 *     all, so this never touches the network. Rising-edge detected
 *     (g_jump_requested) so holding Space doesn't re-jump every
 *     frame; grounded/airborne is tracked explicitly (Camera.jumping)
 *     rather than inferred, since there's no physics engine here to
 *     ask "am I touching the floor" the way Godot's CharacterBody3D
 *     can.
 *   - Ammo/health pickups render as small colored boxes (amber/brass
 *     for ammo, red for health, matching Pickup.gd), hidden while on
 *     cooldown. Purely a rendering addition -- the server already
 *     fully implements pickup/cooldown logic (see net.h's PickupState
 *     and server.c's pickup handling), this client just wasn't
 *     parsing or drawing that data before now.
 *   - A main menu: Play Solo / Play Coop / Quit. GLES2 has no text
 *     rendering (see hud_render.c), so this is a NUMBERED menu
 *     (1/2/3), not a word-based one -- each option is a distinct
 *     color plus its number, the current selection shown at full
 *     brightness and the others dimmed. Navigated with the same
 *     arrow keys used for look in-game and confirmed with the same E
 *     key used for shooting -- safe to reuse since they're read by
 *     entirely different code depending on g_app_state. The network
 *     thread does NOT start until Play Solo/Coop is confirmed here;
 *     Quit exits before ever connecting. "Settings"/"Credits" from
 *     the Godot client aren't here -- Settings would need either
 *     text input (not feasible) or a fully custom numeric-only UI,
 *     and Credits has no functional value without text, so both were
 *     left out rather than built half-working.
 *
 * KNOWN LIMITATIONS carried over / new this stage:
 *   - Mouse look still sticks at the screen edge (see the README);
 *     arrow-key look is the documented workaround, not a fix for that.
 *   - The camera's rendered (x, z) position is now pulled toward the
 *     server's authoritative position every frame (see
 *     reconcile_camera()) -- this turned out to be essential, not
 *     just nice-to-have: without it, zombie AI/hit detection/damage
 *     (all computed server-side against the server's own belief of
 *     where you are) could silently diverge from what's on screen,
 *     which is what "zombie won't chase me / can't hit it / take
 *     damage from nowhere" turned out to be. Height (y) is still
 *     fully local -- the server has no concept of player elevation,
 *     same as always.
 *   - The mouse-button constant used below (SCREEN_LEFT_MOUSE_BUTTON)
 *     is the least-confirmed API name in this file -- if it fails to
 *     compile, checking the actual button-type enum name in your
 *     target's screen.h is a quick, obvious fix (a compile error, not
 *     a silent bug).
 */

#include <screen/screen.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include <sys/keycodes.h>
#include <pthread.h>
#include <sched.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "net.h"
#include "map.h"
#include "qnx_time.h"
#include "mat4.h"
#include "level_geo.h"
#include "hud_render.h"
#include "text_render.h"
#include "font_atlas.h"

#define WINDOW_W 1280   /* fallback only -- actual size is auto-detected from
                         * the display at startup, see main()'s Screen setup */
#define WINDOW_H 720

#define EYE_HEIGHT   1.4f    /* matches src/server.c's PLAYER_EYE_HEIGHT */
#define MOVE_SPEED   3.0f    /* world units/sec -- local free-cam only in
                              * this stage, not yet tied to server MOVE_SPEED */

/* App state -- defined up here since several globals below reference
 * these before the "Menu" section further down. */
#define APP_STATE_MENU    0
#define APP_STATE_PLAYING 1
#define MENU_OPTION_SOLO   0
#define MENU_OPTION_COOP   1
#define MENU_OPTION_QUIT   2
#define MENU_OPTION_COUNT  3
/* Floor/platform height transitions (NOT ramps -- those are fully
 * continuous now, see level_continuous_ramp_height(), and need no
 * easing at all). These only apply to the flat-to-flat step case,
 * e.g. reaching the top of a ramp and stepping onto the platform
 * proper, or walking off a platform edge without jumping. Asymmetric
 * on purpose: falling faster than rising is what reads as "weight"
 * rather than floating up and down at the same rate.
 * COUPLING: mirrors src/server.c's ZOMBIE_CLIMB_SPEED_UP/DOWN, so
 * zombies feel the same "weight" as the player. */
#define CLIMB_SPEED_UP   3.0f
#define CLIMB_SPEED_DOWN 8.0f
#define MOUSE_SENS   0.003f
#define LOOK_SPEED   2.0f    /* radians/sec, arrow-key look */
#define RECONCILE_LERP 0.08f /* per render frame (~60Hz), not per network
                              * tick (20Hz) -- see reconcile_camera()'s
                              * comment for why this is now essential, not
                              * just nice-to-have */
#define GRAVITY       14.0f  /* raised from a "realistic" 9.8 for a
                              * snappier, weightier fall -- matches the
                              * general ask for player/zombie "weight",
                              * not just the walk-off-a-ledge case above */
#define JUMP_VELOCITY 5.0f   /* raised alongside GRAVITY to keep a similar
                              * jump apex (~0.9 units, was ~1.0) despite
                              * the faster fall */
#define PLATFORM_ELEVATED_THRESHOLD (LEVEL_PLATFORM_HEIGHT * 0.6f)
                              /* How far off the ground (not counting
                               * EYE_HEIGHT) the camera has to already be
                               * before a platform tile's footprint counts
                               * as solid ground under your feet, rather
                               * than a ceiling above you -- see
                               * ground_height_for_camera(). Set above the
                               * max reachable jump apex (v^2/2g =~ 1.03
                               * with the constants above) so jumping near
                               * a platform edge can't falsely trigger it. */

/* ------------------------------------------------------------------ */
/* Shared scalar state -- plain volatile, matching the rest of this    */
/* project's cross-thread convention for simple flags/values.          */
/* ------------------------------------------------------------------ */
static volatile int g_running   = 1;
static volatile int g_net_thread_running = 0;   /* separate from g_running --
                                                 * controls just the network
                                                 * thread's own loop, so
                                                 * "return to menu" can stop
                                                 * it without quitting the
                                                 * whole app */
static volatile int g_return_to_menu_requested = 0;   /* ESC while playing --
                                                        * edge-triggered like
                                                        * g_jump_requested */
static volatile int g_app_state = APP_STATE_MENU;   /* readable by
                                                      * handle_keyboard_event
                                                      * so ESC can behave
                                                      * differently in the
                                                      * menu vs. in-game */
static volatile int g_connected = 0;
static volatile u8  g_my_id     = 0xFF;
static volatile f32 g_spawn_x, g_spawn_y, g_spawn_angle;

static volatile int g_key_w = 0, g_key_a = 0, g_key_s = 0, g_key_d = 0;
static volatile int g_key_look_left = 0, g_key_look_right = 0;
static volatile int g_key_look_up = 0, g_key_look_down = 0;
static volatile int g_key_e = 0;   /* fallback shoot key, see handle_keyboard_event */
static volatile int g_key_space = 0;
static volatile int g_jump_requested = 0;   /* set on the down-transition only,
                                             * consumed once by update_camera() --
                                             * avoids re-jumping every frame
                                             * while the key is held */

/* ---- Menu ----
 * GLES2 has no text rendering, and pulling in a font/texture-atlas
 * library was out of scope (same reasoning as hud_render.c's 7-segment
 * digits) -- so the menu is NUMBERED (1/2/3) rather than word-based,
 * navigated with the same arrow keys used for look in-game, and
 * confirmed with the same E key used for shooting in-game. Reusing
 * KEYCODE_UP/DOWN and 'e' for two different purposes is safe since
 * they're read by completely different code paths depending on
 * g_app_state (declared above). */
static volatile int g_menu_nav_up_requested   = 0;
static volatile int g_menu_nav_down_requested = 0;
static volatile int g_menu_confirm_requested  = 0;
static volatile int g_connect_mode = CONNECT_MODE_COOP;   /* set by the menu
                                                            * before the net
                                                            * thread starts */
static volatile int g_mouse_left_down = 0;
static volatile f32 g_cam_yaw = 0.0f, g_cam_pitch = 0.0f;

static int g_have_last_pointer = 0;
static int g_last_pointer_x = 0, g_last_pointer_y = 0;

static int                g_sock = -1;
static struct sockaddr_in g_server_addr;

/* ------------------------------------------------------------------ */
/* Shared entity snapshot -- the one place that gets a real mutex,     */
/* per the reasoning in the file header comment above.                 */
/* ------------------------------------------------------------------ */
typedef struct {
    u8  id;
    int alive;
    f32 x, y;
} RenderPlayer;

typedef struct {
    u8  id;
    int alive;
    u8  type;   /* ZOMBIE_TYPE_NORMAL/TANK/BOSS, from net.h -- picks
                 * color+size in build_entity_geometry(), matching
                 * Zombie.gd's scheme on the Godot side */
    f32 x, y, z;
} RenderZombie;

typedef struct {
    u8  id;
    u8  type;      /* PICKUP_AMMO or PICKUP_HEALTH, from net.h */
    int active;    /* false while on cooldown after being taken */
    f32 x, y;
} RenderPickup;

typedef struct {
    pthread_mutex_t lock;
    RenderPlayer players[NET_MAX_PLAYERS];
    int          player_count;
    RenderZombie zombies[MAX_ZOMBIES];
    int          zombie_count;
    RenderPickup pickups[MAX_PICKUP_SPAWNS];
    int          pickup_count;
    u8           wave;
    int          has_my_state;   /* false until the first PktState with our
                                  * own player entry arrives */
    f32          my_x, my_y;     /* server's authoritative position for THIS
                                  * player -- see reconcile_camera() in main() */
    u8           my_health;
    u8           my_ammo;
} SharedGameState;

static SharedGameState g_state;

/* ------------------------------------------------------------------ */
/* Networking thread                                                    */
/* ------------------------------------------------------------------ */
static void *net_thread_main(void *arg)
{
    u8     buf[1024];
    u32    local_tick = 0;
    double deadline;

    (void)arg;

    deadline = portable_time() + 8.0;
    while (!g_connected && g_net_thread_running && portable_time() < deadline) {
        PktConnect pkt;
        double wait_until;

        memset(&pkt, 0, sizeof(pkt));
        pkt.hdr.type = PKT_CONNECT;
        pkt.hdr.tick = local_tick++;
        pkt.mode     = (u8)g_connect_mode;
        sendto(g_sock, &pkt, sizeof(pkt), 0,
               (struct sockaddr *)&g_server_addr, sizeof(g_server_addr));

        wait_until = portable_time() + 0.5;
        while (g_net_thread_running && !g_connected && portable_time() < wait_until) {
            ssize_t n = recvfrom(g_sock, buf, sizeof(buf), 0, NULL, NULL);
            if (n >= (ssize_t)sizeof(PktHeader)) {
                PktHeader *hdr = (PktHeader *)buf;
                if (hdr->type == PKT_ACCEPT && n >= (ssize_t)sizeof(PktAccept)) {
                    PktAccept *acc = (PktAccept *)buf;
                    g_my_id       = acc->assigned_id;
                    g_spawn_x     = acc->spawn_x;
                    g_spawn_y     = acc->spawn_y;
                    g_spawn_angle = acc->spawn_angle;
                    g_connected   = 1;
                    printf("[net] Connected as player %d, spawn=(%.2f, %.2f)\n",
                           (int)g_my_id, (double)acc->spawn_x, (double)acc->spawn_y);
                }
            }
            portable_sleep_ms(10);
        }
    }

    if (!g_connected) {
        fprintf(stderr, "[net] Failed to connect within 8s\n");
        return NULL;
    }

    while (g_net_thread_running) {
        PktInput inp;
        ssize_t  n;
        int      shoot_held = g_mouse_left_down || g_key_e;

        memset(&inp, 0, sizeof(inp));
        inp.hdr.type         = PKT_INPUT;
        inp.hdr.player_id    = g_my_id;
        inp.hdr.tick         = local_tick++;
        inp.forward          = (u8)g_key_w;
        inp.back             = (u8)g_key_s;
        inp.strafe_left      = (u8)g_key_a;
        inp.strafe_right     = (u8)g_key_d;
        inp.look_angle       = g_cam_yaw;
        inp.pitch            = g_cam_pitch;
        inp.shoot            = (u8)shoot_held;
        sendto(g_sock, &inp, sizeof(inp), 0,
               (struct sockaddr *)&g_server_addr, sizeof(g_server_addr));

        while ((n = recvfrom(g_sock, buf, sizeof(buf), 0, NULL, NULL)) > 0) {
            if (n < (ssize_t)sizeof(PktHeader)) continue;
            PktHeader *hdr = (PktHeader *)buf;
            if (hdr->type == PKT_STATE && n >= (ssize_t)sizeof(PktState)) {
                PktState *st = (PktState *)buf;
                int i, pc = 0, zc = 0;
                u8 my_health = 0, my_ammo = 0;
                f32 my_x = 0.0f, my_y = 0.0f;
                int found_self = 0;

                RenderPlayer tmp_players[NET_MAX_PLAYERS];
                RenderZombie tmp_zombies[MAX_ZOMBIES];
                RenderPickup tmp_pickups[MAX_PICKUP_SPAWNS];
                int pkc = 0;

                for (i = 0; i < (int)st->player_count && i < NET_MAX_PLAYERS; i++) {
                    PlayerState *ps = &st->players[i];
                    if (ps->player_id == g_my_id) {
                        my_health  = ps->health;
                        my_ammo    = ps->ammo;
                        my_x       = ps->x;
                        my_y       = ps->y;
                        found_self = 1;
                        continue;   /* don't render ourselves as an entity */
                    }
                    tmp_players[pc].id    = ps->player_id;
                    tmp_players[pc].alive = ps->alive;
                    tmp_players[pc].x     = ps->x;
                    tmp_players[pc].y     = ps->y;
                    pc++;
                }
                for (i = 0; i < (int)st->zombie_count && i < MAX_ZOMBIES; i++) {
                    ZombieState *zs = &st->zombies[i];
                    tmp_zombies[zc].id    = zs->zombie_id;
                    tmp_zombies[zc].alive = zs->alive;
                    tmp_zombies[zc].type  = zs->type;
                    tmp_zombies[zc].x     = zs->x;
                    tmp_zombies[zc].y     = zs->y;
                    tmp_zombies[zc].z     = zs->z;
                    zc++;
                }
                for (i = 0; i < (int)st->pickup_count && i < MAX_PICKUP_SPAWNS; i++) {
                    PickupState *pk = &st->pickups[i];
                    tmp_pickups[pkc].id     = pk->pickup_id;
                    tmp_pickups[pkc].type   = pk->type;
                    tmp_pickups[pkc].active = pk->active;
                    tmp_pickups[pkc].x      = pk->x;
                    tmp_pickups[pkc].y      = pk->y;
                    pkc++;
                }

                pthread_mutex_lock(&g_state.lock);
                memcpy(g_state.players, tmp_players, sizeof(RenderPlayer) * (size_t)pc);
                g_state.player_count = pc;
                memcpy(g_state.zombies, tmp_zombies, sizeof(RenderZombie) * (size_t)zc);
                g_state.zombie_count = zc;
                memcpy(g_state.pickups, tmp_pickups, sizeof(RenderPickup) * (size_t)pkc);
                g_state.pickup_count = pkc;
                g_state.wave      = st->wave;
                g_state.my_health = my_health;
                g_state.my_ammo   = my_ammo;
                if (found_self) {
                    g_state.my_x = my_x;
                    g_state.my_y = my_y;
                    g_state.has_my_state = 1;
                }
                pthread_mutex_unlock(&g_state.lock);
            }
        }
        portable_sleep_ms(50);
    }
    return NULL;
}

static int start_net_thread_with_scheduling(pthread_t *out_tid)
{
    int min_fifo = sched_get_priority_min(SCHED_FIFO);
    int max_fifo = sched_get_priority_max(SCHED_FIFO);
    /* Networking gets priority >= rendering, not lower -- a dropped
     * frame is a minor cosmetic issue, but if rendering ever fails to
     * yield the CPU as expected (relies on eglSwapBuffers() actually
     * blocking for vsync; SCREEN_PROPERTY_SWAP_INTERVAL is set to 1
     * for exactly this reason, but a real-time SCHED_FIFO thread that
     * doesn't yield will always starve anything lower-priority
     * regardless of *why* it isn't yielding), the network thread must
     * still be able to preempt it and keep the connection alive. The
     * previous ordering (render=max, net=mid) had this backwards:
     * correctness-critical networking was the one that could be
     * starved, not the purely cosmetic render loop. */
    int net_prio    = max_fifo;
    int render_prio = (max_fifo + min_fifo) / 2;

    struct sched_param render_sp;
    pthread_attr_t     net_attr;
    struct sched_param net_sp;
    int rc;

    render_sp.sched_priority = render_prio;
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &render_sp) != 0)
        perror("[sched] pthread_setschedparam (render/main thread)");
    else
        printf("[sched] render thread: SCHED_FIFO priority %d\n", render_prio);

    pthread_attr_init(&net_attr);
    pthread_attr_setinheritsched(&net_attr, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&net_attr, SCHED_FIFO);
    net_sp.sched_priority = net_prio;
    pthread_attr_setschedparam(&net_attr, &net_sp);

    rc = pthread_create(out_tid, &net_attr, net_thread_main, NULL);
    if (rc != 0) {
        /* Retry with default scheduling rather than leaving
         * networking entirely broken -- SCHED_FIFO thread creation
         * can fail outright without the right privilege (e.g.
         * PROCMGR_AID_PRIORITY on QNX, or an rlimit on Linux), and a
         * plain thread is far better than no thread at all for
         * something this central to the game actually working. */
        fprintf(stderr, "[sched] pthread_create with SCHED_FIFO (net thread) "
                        "failed: %d -- retrying with default scheduling\n", rc);
        rc = pthread_create(out_tid, NULL, net_thread_main, NULL);
        if (rc != 0)
            fprintf(stderr, "[sched] pthread_create (net thread) failed even "
                            "with default scheduling: %d\n", rc);
        else
            printf("[sched] net thread: default scheduling (SCHED_FIFO unavailable)\n");
    } else {
        printf("[sched] net thread: SCHED_FIFO priority %d\n", net_prio);
    }
    pthread_attr_destroy(&net_attr);
    return rc;
}

/* ------------------------------------------------------------------ */
/* GLES2 shader pipeline.                                              */
/*                                                                      */
/* Two separate programs share the SAME GeoVertex buffer layout        */
/* (pos+normal+color, 36 bytes/vertex):                                */
/*   - "simple": flat per-vertex color, used for entities and the HUD/ */
/*     menu (a zombie or a HUD digit shouldn't look like a brick wall, */
/*     and 2D screen-space UI has no meaningful "normal" anyway).      */
/*   - "level": adds a procedural brick pattern on wall-like faces,    */
/*     used ONLY for the static level_vbo draw call. Distinguishing    */
/*     "wall" from "floor/ceiling/platform" happens via the vertex     */
/*     normal (|normal.y| close to 1 = horizontal-ish surface, close   */
/*     to 0 = vertical wall face) rather than needing a whole second   */
/*     vertex buffer or a per-vertex flag.                             */
/*                                                                      */
/* No texture image anywhere -- the brick pattern is generated purely  */
/* from world position math in the fragment shader, same "zero asset   */
/* pipeline" approach as the rest of this renderer (7-segment HUD       */
/* digits, procedural ramp meshes, etc.). Inspired by QNX's own         */
/* gles2-maze sample (uniform mat4 mvp; attribute position/color;      */
/* varying color; gl_FragColor = vcolor * texture2D(...)) -- same       */
/* "multiply a base color by a per-fragment pattern" idea, just with    */
/* a computed pattern instead of a sampled image.                       */
/* ------------------------------------------------------------------ */
static const char *VERTEX_SHADER_SRC =
    "attribute vec3 a_position;\n"
    "attribute vec3 a_color;\n"
    "uniform mat4 u_mvp;\n"
    "varying vec3 v_color;\n"
    "void main() {\n"
    "    gl_Position = u_mvp * vec4(a_position, 1.0);\n"
    "    v_color = a_color;\n"
    "}\n";

static const char *FRAGMENT_SHADER_SRC =
    "precision mediump float;\n"
    "varying vec3 v_color;\n"
    "void main() {\n"
    "    gl_FragColor = vec4(v_color, 1.0);\n"
    "}\n";

static const char *LEVEL_VERTEX_SHADER_SRC =
    "attribute vec3 a_position;\n"
    "attribute vec3 a_normal;\n"
    "attribute vec3 a_color;\n"
    "uniform mat4 u_mvp;\n"
    "varying vec3 v_color;\n"
    "varying vec3 v_normal;\n"
    "varying vec3 v_world_pos;\n"
    "void main() {\n"
    "    gl_Position = u_mvp * vec4(a_position, 1.0);\n"
    "    v_color = a_color;\n"
    "    v_normal = a_normal;\n"
    "    v_world_pos = a_position;\n"
    "}\n";

static const char *LEVEL_FRAGMENT_SHADER_SRC =
    "precision mediump float;\n"
    "varying vec3 v_color;\n"
    "varying vec3 v_normal;\n"
    "varying vec3 v_world_pos;\n"
    "void main() {\n"
    "    float wallness = 1.0 - abs(v_normal.y);\n"
    /* Pseudo-UV for axis-aligned box geometry: every wall face has
     * exactly one of world x/z roughly constant across the face and
     * the other varying along its length -- summing them gives a
     * cheap single coordinate that varies correctly along the wall
     * regardless of whether it runs north-south or east-west, with no
     * per-vertex UV attribute needed. */
    "    float u = v_world_pos.x + v_world_pos.z;\n"
    "    float v = v_world_pos.y;\n"
    "    float brick_w = 1.0;\n"
    "    float brick_h = 0.5;\n"
    "    float mortar = 0.06;\n"
    "    float row = floor(v / brick_h);\n"
    "    float row_offset = mod(row, 2.0) * 0.5 * brick_w;\n"
    "    float bx = fract((u + row_offset) / brick_w);\n"
    "    float by = fract(v / brick_h);\n"
    "    float mortar_line = clamp(step(bx, mortar) + step(1.0 - mortar, bx) +\n"
    "                               step(by, mortar) + step(1.0 - mortar, by), 0.0, 1.0);\n"
    /* cheap per-brick tint variation so bricks aren't perfectly
     * uniform -- a sine-based hash, not real noise, but enough to
     * break up the flatness the person asked about */
    "    float brick_id = floor((u + row_offset) / brick_w) + row * 13.0;\n"
    "    float tint = 0.92 + 0.08 * fract(sin(brick_id * 12.9898) * 43758.5453);\n"
    "    vec3 brick_color = v_color * tint * (1.0 - mortar_line * 0.55);\n"
    "    vec3 final_color = mix(v_color, brick_color, wallness);\n"
    "    gl_FragColor = vec4(final_color, 1.0);\n"
    "}\n";

/* Third program, text only -- see text_render.h for why this needs
 * its own vertex format (position+texcoord+color, no normal) rather
 * than reusing either of the two above. Position is already NDC
 * (text_render.c converts on the CPU side, same convention as
 * hud_render.c's 2D elements), so no MVP multiply is needed here at
 * all -- simpler than carrying an always-identity uniform through. */
static const char *TEXT_VERTEX_SHADER_SRC =
    "attribute vec2 a_position;\n"
    "attribute vec2 a_texcoord;\n"
    "attribute vec3 a_color;\n"
    "varying vec2 v_texcoord;\n"
    "varying vec3 v_color;\n"
    "void main() {\n"
    "    gl_Position = vec4(a_position, 0.0, 1.0);\n"
    "    v_texcoord = a_texcoord;\n"
    "    v_color = a_color;\n"
    "}\n";

static const char *TEXT_FRAGMENT_SHADER_SRC =
    "precision mediump float;\n"
    "uniform sampler2D u_tex;\n"
    "varying vec2 v_texcoord;\n"
    "varying vec3 v_color;\n"
    "void main() {\n"
    /* Hard cutout rather than smooth alpha blending -- the atlas is
     * essentially binary coverage already (see make_font_atlas.py's
     * verified pixel-value check), and this avoids needing to enable
     * GL_BLEND / manage draw order anywhere else in this renderer,
     * which uses depth testing for everything else instead. */
    "    float a = texture2D(u_tex, v_texcoord).a;\n"
    "    if (a < 0.5) discard;\n"
    "    gl_FragColor = vec4(v_color, 1.0);\n"
    "}\n";

static GLuint compile_shader(GLenum type, const char *src)
{
    GLuint shader = glCreateShader(type);
    GLint  ok;
    glShaderSource(shader, 1, &src, NULL);
    glCompileShader(shader);
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        fprintf(stderr, "[gl] shader compile failed: %s\n", log);
        return 0;
    }
    return shader;
}

static GLuint build_shader_program(const char *vs_src, const char *fs_src, GLint *out_mvp_loc)
{
    GLuint vs, fs, prog;
    GLint  ok;

    vs = compile_shader(GL_VERTEX_SHADER, vs_src);
    fs = compile_shader(GL_FRAGMENT_SHADER, fs_src);
    if (!vs || !fs) return 0;

    prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(prog, sizeof(log), NULL, log);
        fprintf(stderr, "[gl] program link failed: %s\n", log);
        return 0;
    }

    *out_mvp_loc = glGetUniformLocation(prog, "u_mvp");
    return prog;
}

/* GeoVertex is laid out {pos(3), normal(3), color(3)} regardless of
 * which program draws it -- these two draw functions differ only in
 * whether they also bind a_normal, not in the underlying buffer. */
static void draw_vertex_list(GLuint vbo, GLint pos_loc, GLint color_loc, int vertex_count)
{
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glVertexAttribPointer((GLuint)pos_loc, 3, GL_FLOAT, GL_FALSE,
                          sizeof(GeoVertex), (const void *)0);
    glEnableVertexAttribArray((GLuint)pos_loc);
    glVertexAttribPointer((GLuint)color_loc, 3, GL_FLOAT, GL_FALSE,
                          sizeof(GeoVertex), (const void *)(6 * sizeof(f32)));
    glEnableVertexAttribArray((GLuint)color_loc);
    glDrawArrays(GL_TRIANGLES, 0, vertex_count);
}

static void draw_vertex_list_lit(GLuint vbo, GLint pos_loc, GLint normal_loc, GLint color_loc,
                                 int vertex_count)
{
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glVertexAttribPointer((GLuint)pos_loc, 3, GL_FLOAT, GL_FALSE,
                          sizeof(GeoVertex), (const void *)0);
    glEnableVertexAttribArray((GLuint)pos_loc);
    glVertexAttribPointer((GLuint)normal_loc, 3, GL_FLOAT, GL_FALSE,
                          sizeof(GeoVertex), (const void *)(3 * sizeof(f32)));
    glEnableVertexAttribArray((GLuint)normal_loc);
    glVertexAttribPointer((GLuint)color_loc, 3, GL_FLOAT, GL_FALSE,
                          sizeof(GeoVertex), (const void *)(6 * sizeof(f32)));
    glEnableVertexAttribArray((GLuint)color_loc);
    glDrawArrays(GL_TRIANGLES, 0, vertex_count);
}

/* Uploads a TextVertexList to text_vbo and draws it -- unlike the two
 * functions above, this one also binds the font atlas texture (text
 * always uses exactly one texture, so there's no point taking it as
 * a parameter the way vbo/locs are). Caller is responsible for
 * glUseProgram(text_prog) beforehand, same convention as the other
 * two draw functions relying on the right program already being
 * active. */
static void draw_text(GLuint text_vbo, GLuint font_texture, GLint pos_loc,
                      GLint texcoord_loc, GLint color_loc, GLint tex_loc,
                      const TextVertexList *tl)
{
    if (tl->count <= 0) return;
    glBindBuffer(GL_ARRAY_BUFFER, text_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof(TextVertex) * (size_t)tl->count),
                tl->verts, GL_DYNAMIC_DRAW);
    glVertexAttribPointer((GLuint)pos_loc, 2, GL_FLOAT, GL_FALSE,
                          sizeof(TextVertex), (const void *)0);
    glEnableVertexAttribArray((GLuint)pos_loc);
    glVertexAttribPointer((GLuint)texcoord_loc, 2, GL_FLOAT, GL_FALSE,
                          sizeof(TextVertex), (const void *)(2 * sizeof(f32)));
    glEnableVertexAttribArray((GLuint)texcoord_loc);
    glVertexAttribPointer((GLuint)color_loc, 3, GL_FLOAT, GL_FALSE,
                          sizeof(TextVertex), (const void *)(4 * sizeof(f32)));
    glEnableVertexAttribArray((GLuint)color_loc);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, font_texture);
    glUniform1i(tex_loc, 0);

    glDrawArrays(GL_TRIANGLES, 0, tl->count);
}
/* ------------------------------------------------------------------ */
typedef struct {
    f32 x, y, z;
    f32 yaw, pitch;
    f32 vel_y;      /* vertical velocity, for jumping -- 0 while grounded */
    int jumping;    /* true from the moment of a jump until landing again */
} Camera;

static void handle_keyboard_event(screen_event_t ev, Camera *cam)
{
    int flags = 0, sym = 0, down, lower;
    (void)cam;
    screen_get_event_property_iv(ev, SCREEN_PROPERTY_FLAGS, &flags);
    screen_get_event_property_iv(ev, SCREEN_PROPERTY_SYM, &sym);
    down  = (flags & KEY_DOWN) ? 1 : 0;
    lower = sym | 0x20;   /* normalizes plain ASCII letters to lowercase */

    if (lower == 'w') g_key_w = down;
    else if (lower == 'a') g_key_a = down;
    else if (lower == 's') g_key_s = down;
    else if (lower == 'd') g_key_d = down;
    else if (lower == 'e') {
        if (down && !g_key_e) g_menu_confirm_requested = 1;   /* menu confirm --
                                                               * only read while
                                                               * g_app_state ==
                                                               * APP_STATE_MENU */
        g_key_e = down;   /* fallback shoot -- plain ASCII letter like WASD,
                          * unlike Enter which turned out not to report as
                          * plain '\r'/'\n' on real hardware */
    }
    else if (sym == ' ') {   /* jump -- ASCII 0x20, plain printable char like WASD */
        if (down && !g_key_space) g_jump_requested = 1;   /* rising edge only */
        g_key_space = down;
    }
    else if (sym == KEYCODE_LEFT)  g_key_look_left  = down;
    else if (sym == KEYCODE_RIGHT) g_key_look_right = down;
    else if (sym == KEYCODE_UP) {
        if (down && !g_key_look_up) g_menu_nav_up_requested = 1;   /* menu nav --
                                                                    * only read
                                                                    * in APP_STATE_MENU */
        g_key_look_up = down;
    }
    else if (sym == KEYCODE_DOWN) {
        if (down && !g_key_look_down) g_menu_nav_down_requested = 1;
        g_key_look_down = down;
    }
    else if (sym == KEYCODE_ESCAPE && down) {
        if (g_app_state == APP_STATE_PLAYING) g_return_to_menu_requested = 1;
        else g_running = 0;   /* ESC while already in the menu still quits the app */
    }
}

static void handle_pointer_event(screen_event_t ev, Camera *cam)
{
    int pos[2] = { 0, 0 };
    int buttons = 0;

    screen_get_event_property_iv(ev, SCREEN_PROPERTY_POSITION, pos);
    screen_get_event_property_iv(ev, SCREEN_PROPERTY_BUTTONS, &buttons);
    g_mouse_left_down = (buttons & SCREEN_LEFT_MOUSE_BUTTON) ? 1 : 0;

    if (g_have_last_pointer) {
        int dx = pos[0] - g_last_pointer_x;
        int dy = pos[1] - g_last_pointer_y;
        cam->yaw   -= (f32)dx * MOUSE_SENS;
        cam->pitch -= (f32)dy * MOUSE_SENS;
        if (cam->pitch >  1.4f) cam->pitch =  1.4f;
        if (cam->pitch < -1.4f) cam->pitch = -1.4f;
    }
    g_last_pointer_x = pos[0];
    g_last_pointer_y = pos[1];
    g_have_last_pointer = 1;
}

/* Moves the camera per the currently-held WASD/arrow keys, with wall
 * collision via map_is_wall() and ramp/platform height easing --
 * unchanged from Stage 2a. Also mirrors the resulting look angle into
 * the shared g_cam_yaw/g_cam_pitch so the network thread can send
 * real look angles.
 *
 * COUPLING WARNING, and the actual root cause of "zombie won't die /
 * WASD feels random": this client's own `cam->yaw` is just an angle
 * this file uses internally to build the forward/right vectors below
 * -- it has no reason to already match src/server.c's own convention,
 * where facing direction is computed as (cosf(angle), sinf(angle)).
 * Sending cam->yaw to the server directly (which the first version of
 * this file did) means the server computes a DIFFERENT "forward" than
 * what's actually rendered -- a fixed rotational mismatch. Since
 * shooting/hit-detection use the server's own angle, and since
 * reconcile_camera() pulls this camera toward wherever the server
 * moved the player using that same (wrong) angle, the visible result
 * was exactly what got reported: shots aimed at a visually-correct
 * target still missed server-side, and WASD felt like it was fighting
 * itself as reconciliation pulled the camera toward a server position
 * that had moved in a rotated direction relative to the actual input.
 * Fix: derive the angle actually sent to the server from the already-
 * computed forward vector via atan2 (see the end of this function),
 * the same technique used for the Godot client's Player.gd for the
 * identical reason -- self-consistent by construction instead of
 * hand-deriving the exact offset between two conventions. */
/* Computes the floor height (not including EYE_HEIGHT) the camera's
 * feet should be at, given its CURRENT floor-relative height.
 *   - Ramp tiles: the exact CONTINUOUS position-based height (see
 *     level_continuous_ramp_height()) -- always correct regardless of
 *     current height, since a ramp is inherently "the sloped surface
 *     here". The caller (update_camera) snaps directly to this, no
 *     easing, which is what actually fixes the climbing-lag bug.
 *   - Platform tiles: gated by current height -- only treated as
 *     solid ground if already substantially elevated (i.e. arrived
 *     via the ramp), otherwise this is "walking under the platform",
 *     and the floor here is still 0. Without this gate, merely
 *     walking into a platform's horizontal footprint from below would
 *     incorrectly pull the camera straight up onto it.
 *   - Everything else: flat floor, 0. */
static f32 target_floor_height(f32 x, f32 y, f32 current_floor_height)
{
    int mx = (int)x, my = (int)y;
    int t = map_tile(mx, my);
    if (t == TILE_RAMP)     return level_continuous_ramp_height(x, y);
    if (t == TILE_PLATFORM) return (current_floor_height > PLATFORM_ELEVATED_THRESHOLD)
                                  ? LEVEL_PLATFORM_HEIGHT : 0.0f;
    return 0.0f;
}

static void update_camera(Camera *cam, f32 dt)
{
    f32 fx, fz, rx, rz;
    f32 mvx = 0.0f, mvz = 0.0f;
    f32 len;
    f32 new_x, new_z, target_y;

    if (g_key_look_left)  cam->yaw += LOOK_SPEED * dt;
    if (g_key_look_right) cam->yaw -= LOOK_SPEED * dt;
    if (g_key_look_up)    cam->pitch += LOOK_SPEED * dt;
    if (g_key_look_down)  cam->pitch -= LOOK_SPEED * dt;
    if (cam->pitch >  1.4f) cam->pitch =  1.4f;
    if (cam->pitch < -1.4f) cam->pitch = -1.4f;

    fx = -sinf(cam->yaw); fz = -cosf(cam->yaw);
    rx =  cosf(cam->yaw); rz = -sinf(cam->yaw);

    if (g_key_w) { mvx += fx; mvz += fz; }
    if (g_key_s) { mvx -= fx; mvz -= fz; }
    if (g_key_d) { mvx += rx; mvz += rz; }
    if (g_key_a) { mvx -= rx; mvz -= rz; }

    len = sqrtf(mvx * mvx + mvz * mvz);
    if (len > 0.0001f) {
        f32 speed = MOVE_SPEED * dt;
        mvx = mvx / len * speed;
        mvz = mvz / len * speed;

        new_x = cam->x + mvx;
        new_z = cam->z + mvz;
        if (!map_is_wall((int)new_x, (int)cam->z)) cam->x = new_x;
        if (!map_is_wall((int)cam->x, (int)new_z)) cam->z = new_z;
    }

    target_y = target_floor_height(cam->x, cam->z, cam->y - EYE_HEIGHT) + EYE_HEIGHT;

    if (!cam->jumping) {
        if (map_tile((int)cam->x, (int)cam->z) == TILE_RAMP) {
            /* Continuous target -- nothing to catch up to, so snap
             * directly. This is the actual climbing-lag fix: easing
             * toward a target that itself moves smoothly with
             * position just adds unnecessary (and, at the old rate,
             * too-slow) lag on top of an already-smooth function. */
            cam->y = target_y;
        } else {
            /* Floor/platform: a real discrete step (e.g. reaching the
             * platform proper, or walking off its edge), eased
             * asymmetrically for the "weight" feel -- see
             * CLIMB_SPEED_UP/DOWN. */
            if (cam->y < target_y) {
                cam->y += CLIMB_SPEED_UP * dt;
                if (cam->y > target_y) cam->y = target_y;
            } else if (cam->y > target_y) {
                cam->y -= CLIMB_SPEED_DOWN * dt;
                if (cam->y < target_y) cam->y = target_y;
            }
        }

        if (g_jump_requested) {
            g_jump_requested = 0;
            cam->vel_y  = JUMP_VELOCITY;
            cam->jumping = 1;
        }
    } else {
        /* Airborne: real gravity, not the easing above -- a jump
         * needs an actual arc, not a constant-speed glide. */
        cam->vel_y -= GRAVITY * dt;
        cam->y     += cam->vel_y * dt;

        if (cam->y <= target_y) {
            cam->y      = target_y;   /* landed */
            cam->vel_y  = 0.0f;
            cam->jumping = 0;
        }
    }

    g_cam_yaw   = atan2f(fz, fx);   /* NOT cam->yaw directly -- see the
                                     * COUPLING WARNING at the top of this
                                     * function for why */
    g_cam_pitch = cam->pitch;
}

/* Pulls the camera's (x, z) toward the server's authoritative position
 * for this player -- NOT y (height), which stays fully local (ramp/
 * platform climbing has no server-side equivalent for players, same
 * as the Godot client's reconciliation preserving local Y).
 *
 * This is not just cosmetic smoothing: zombie AI, hit detection, and
 * damage are ALL computed server-side against the server's own (x,y)
 * for this player, not against wherever this camera is actually
 * rendered. Without this pull, the two positions can drift arbitrarily
 * far apart over time (different movement speed constants, different
 * wall-collision edge cases, etc.), which is what "zombie doesn't
 * chase me / my shots don't land / I take damage from nowhere" all
 * turned out to be -- the server was correctly simulating a player
 * standing somewhere this camera wasn't. */
static void reconcile_camera(Camera *cam)
{
    int has_state;
    f32 server_x, server_y;

    pthread_mutex_lock(&g_state.lock);
    has_state = g_state.has_my_state;
    server_x  = g_state.my_x;
    server_y  = g_state.my_y;
    pthread_mutex_unlock(&g_state.lock);

    if (!has_state) return;   /* haven't heard from the server yet */

    cam->x += (server_x - cam->x) * RECONCILE_LERP;
    cam->z += (server_y - cam->z) * RECONCILE_LERP;
}

/* Builds a fresh vertex list for every zombie/other-player in the
 * latest shared snapshot -- called once per frame, unlike the level's
 * static geometry which is built once at startup. */
static VertexList build_entity_geometry(void)
{
    VertexList vl;
    RenderPlayer players[NET_MAX_PLAYERS];
    RenderZombie zombies[MAX_ZOMBIES];
    RenderPickup pickups[MAX_PICKUP_SPAWNS];
    int player_count, zombie_count, pickup_count, i;

    memset(&vl, 0, sizeof(vl));

    pthread_mutex_lock(&g_state.lock);
    player_count = g_state.player_count;
    zombie_count = g_state.zombie_count;
    pickup_count = g_state.pickup_count;
    memcpy(players, g_state.players, sizeof(RenderPlayer) * (size_t)player_count);
    memcpy(zombies, g_state.zombies, sizeof(RenderZombie) * (size_t)zombie_count);
    memcpy(pickups, g_state.pickups, sizeof(RenderPickup) * (size_t)pickup_count);
    pthread_mutex_unlock(&g_state.lock);

    for (i = 0; i < player_count; i++) {
        if (!players[i].alive) continue;
        push_box(&vl, players[i].x, 0.8f, players[i].y,
                 0.3f, 0.8f, 0.3f,
                 0.9f, 0.75f, 0.1f);   /* yellow -- teammate, matches RemotePlayer.gd */
    }
    for (i = 0; i < zombie_count; i++) {
        f32 scale, r, g, b, half_h, half_r;
        if (!zombies[i].alive) continue;

        /* Matches Zombie.gd's BASE_HEIGHT=1.8/BASE_RADIUS=0.35 and its
         * TANK_SCALE/BOSS_SCALE/COLOR_* constants exactly, so a tank
         * or boss looks the same size and color on both clients. */
        switch (zombies[i].type) {
            case ZOMBIE_TYPE_TANK:
                scale = 1.3f; r = 0.75f; g = 0.15f; b = 0.15f; break;
            case ZOMBIE_TYPE_BOSS:
                scale = 1.7f; r = 0.15f; g = 0.35f; b = 0.85f; break;
            default:
                scale = 1.0f; r = 0.25f; g = 0.55f; b = 0.20f; break;
        }
        half_h = 0.9f * scale;
        half_r = 0.35f * scale;
        push_box(&vl, zombies[i].x, zombies[i].z + half_h, zombies[i].y,
                 half_r, half_h, half_r, r, g, b);
    }
    for (i = 0; i < pickup_count; i++) {
        if (!pickups[i].active) continue;   /* on cooldown -- hidden, matches Pickup.gd */
        if (pickups[i].type == PICKUP_AMMO) {
            push_box(&vl, pickups[i].x, 0.9f, pickups[i].y,
                     0.175f, 0.175f, 0.175f,
                     0.9f, 0.7f, 0.15f);    /* amber/brass, matches Pickup.gd */
        } else {
            push_box(&vl, pickups[i].x, 0.9f, pickups[i].y,
                     0.175f, 0.175f, 0.175f,
                     0.85f, 0.15f, 0.2f);   /* red cross-ish, matches Pickup.gd */
        }
    }

    return vl;
}

/* Builds the HUD overlay for the current frame: crosshair + health/
 * ammo (as "current / max", using a lit "1" as the separator -- see
 * the caller's note on why) + wave/zombies-left as plain numbers.
 *
 * COUPLING WARNING: PLAYER_MAX_HEALTH/PLAYER_MAX_AMMO must match
 * src/server.c's initial health (100) and AMMO_MAX. The server never
 * sends a "max" value over the wire, only current -- these are only
 * used here to know what to show on the right of the "/". */
#define PLAYER_MAX_HEALTH 100
#define PLAYER_MAX_AMMO   60

/* Renders "current [1-as-separator] max" starting at (px, py) --
 * chains three hud_push_number() calls, using hud_number_width() to
 * space them correctly regardless of digit count. The separator is
 * dimmer than the two numbers so it reads as a divider rather than a
 * third value, on top of the "1" glyph's own vertical-bar shape
 * already looking slash-like between two numbers. */
static f32 push_stat_ratio(VertexList *vl, int current, int max, f32 px, f32 py,
                          f32 digit_w, f32 digit_h, int win_w, int win_h,
                          f32 r, f32 g, f32 b)
{
    f32 x = px;
    hud_push_number(vl, current, x, py, digit_w, digit_h, win_w, win_h, r, g, b);
    x += hud_number_width(current, digit_w);
    hud_push_number(vl, 1, x, py, digit_w, digit_h, win_w, win_h, r * 0.6f, g * 0.6f, b * 0.6f);
    x += hud_number_width(1, digit_w);
    hud_push_number(vl, max, x, py, digit_w, digit_h, win_w, win_h, r, g, b);
    x += hud_number_width(max, digit_w);
    return x - px;   /* total width, for right-aligning callers */
}

static VertexList build_hud_geometry(int win_w, int win_h)
{
    VertexList vl;
    u8  health, ammo, wave;
    int zombie_count;
    const f32 digit_w = 20.0f, digit_h = 32.0f;
    f32 ammo_width;

    memset(&vl, 0, sizeof(vl));

    pthread_mutex_lock(&g_state.lock);
    health       = g_state.my_health;
    ammo         = g_state.my_ammo;
    wave         = g_state.wave;
    zombie_count = g_state.zombie_count;
    pthread_mutex_unlock(&g_state.lock);

    hud_push_crosshair(&vl, win_w, win_h, 1.0f, 1.0f, 1.0f);

    /* bottom-left: health, e.g. "100/100" */
    push_stat_ratio(&vl, (int)health, PLAYER_MAX_HEALTH, 24.0f, (f32)win_h - 56.0f,
                    digit_w, digit_h, win_w, win_h, 0.2f, 0.9f, 0.2f);

    /* bottom-right: ammo, e.g. "60/60" -- right-aligned using the
     * computed width so the right edge stays fixed regardless of how
     * many digits the current value has (measured with a throwaway
     * VertexList first, since we need the width before knowing where
     * to actually start drawing). */
    {
        VertexList measure;
        memset(&measure, 0, sizeof(measure));
        ammo_width = push_stat_ratio(&measure, (int)ammo, PLAYER_MAX_AMMO, 0.0f, 0.0f,
                                     digit_w, digit_h, win_w, win_h, 0, 0, 0);
        vertex_list_free(&measure);
    }
    push_stat_ratio(&vl, (int)ammo, PLAYER_MAX_AMMO,
                    (f32)win_w - 24.0f - ammo_width, (f32)win_h - 56.0f,
                    digit_w, digit_h, win_w, win_h, 0.9f, 0.7f, 0.15f);

    /* top-right: wave */
    hud_push_number(&vl, (int)wave, (f32)win_w - 140.0f, 24.0f, 20.0f, 32.0f,
                    win_w, win_h, 0.8f, 0.8f, 0.9f);
    /* top-left: zombies remaining */
    hud_push_number(&vl, zombie_count, 24.0f, 24.0f, 20.0f, 32.0f,
                    win_w, win_h, 0.9f, 0.3f, 0.3f);

    return vl;
}

/* Builds the main menu: three numbered, color-coded selection boxes
 * (1=Solo, 2=Coop, 3=Quit), stacked vertically and horizontally
 * centered on win_w -- no text anywhere, per the GLES2-has-no-font-
 * rendering constraint noted at the top of this file; the number
 * inside each box plus its distinct color is what distinguishes the
 * options. The current selection is shown at full brightness, the
 * other two dimmed, rather than drawing a separate border/highlight
 * box -- simpler, and avoids the exact seam/z-fighting-in-2D
 * questions a separate outline box would raise. */
static VertexList build_menu_geometry(int win_w, int win_h, int selection, TextVertexList *out_text)
{
    VertexList vl;
    const f32 box_w = 320.0f, box_h = 70.0f, spacing = 100.0f;
    const f32 base_colors[MENU_OPTION_COUNT][3] = {
        { 0.25f, 0.70f, 0.30f },   /* 1: Solo -- green */
        { 0.25f, 0.45f, 0.85f },   /* 2: Coop -- blue */
        { 0.65f, 0.20f, 0.20f },   /* 3: Quit -- red */
    };
    static const char *labels[MENU_OPTION_COUNT] = { "SOLO", "COOP", "QUIT" };
    f32 first_top = (f32)win_h * 0.35f;
    f32 cx = (f32)win_w * 0.5f;
    int i;

    memset(&vl, 0, sizeof(vl));
    memset(out_text, 0, sizeof(*out_text));

    /* Title -- centered above the option boxes. This is the first
     * actual text anywhere in this client; everything before this
     * (HUD health/ammo/wave, menu options) had to make do with
     * hud_render.c's 7-segment digits since there was no font
     * rendering at all. See text_render.h / font_atlas.h. */
    {
        const char *title = "DOOM QNX COOP";
        f32 title_char_w = 28.0f, title_char_h = 40.0f;
        f32 title_w = text_string_width(title, title_char_w);
        f32 title_top = first_top - 90.0f;
        text_push_string(out_text, title, cx - title_w * 0.5f, title_top,
                         title_char_w, title_char_h, win_w, win_h, 0.85f, 0.85f, 0.95f);
    }

    for (i = 0; i < MENU_OPTION_COUNT; i++) {
        f32 top    = first_top + (f32)i * spacing;
        f32 bottom = top + box_h;
        f32 left   = cx - box_w * 0.5f;
        f32 right  = cx + box_w * 0.5f;
        f32 dim    = (i == selection) ? 1.0f : 0.45f;   /* selected = full
                                                          * brightness */
        f32 r = base_colors[i][0] * dim;
        f32 g = base_colors[i][1] * dim;
        f32 b = base_colors[i][2] * dim;
        f32 label_char_w = 24.0f, label_char_h = 34.0f;
        f32 label_w = text_string_width(labels[i], label_char_w);

        hud_push_quad(&vl, left, top, right, bottom, win_w, win_h, r, g, b);

        /* Number, left-aligned inside the box, vertically centered --
         * kept alongside the new text label as a quick-select hint
         * (matches whatever key you'd press), not replaced by it. */
        hud_push_digit(&vl, i + 1, left + 24.0f, top + (box_h - 40.0f) * 0.5f,
                       24.0f, 40.0f, win_w, win_h, 1.0f, 1.0f, 1.0f);

        /* Text label, centered in the remaining space to the right of
         * the number. */
        text_push_string(out_text, labels[i], cx + 20.0f - label_w * 0.5f,
                         top + (box_h - label_char_h) * 0.5f,
                         label_char_w, label_char_h, win_w, win_h, 1.0f, 1.0f, 1.0f);
    }

    return vl;
}

/* ------------------------------------------------------------------ */
/* main                                                                 */
/* ------------------------------------------------------------------ */
int main(int argc, char **argv)
{
    const char *server_ip = (argc > 1) ? argv[1] : "127.0.0.1";
    u16         port      = (argc > 2) ? (u16)atoi(argv[2]) : NET_PORT;

    screen_context_t screen_ctx;
    screen_window_t  screen_win;
    screen_event_t   screen_ev;

    EGLDisplay egl_disp;
    EGLConfig  egl_config;
    EGLContext egl_ctx;
    EGLSurface egl_surf;
    EGLint     num_config;

    pthread_t net_tid;

    GLuint  prog, level_prog, text_prog;
    GLint   pos_loc, color_loc, mvp_loc;
    GLint   level_pos_loc, level_normal_loc, level_color_loc, level_mvp_loc;
    GLint   text_pos_loc, text_texcoord_loc, text_color_loc, text_tex_loc;
    GLuint  level_vbo, entity_vbo, hud_vbo, text_vbo;
    GLuint  font_texture;
    int     level_vertex_count;
    VertexList level_geo;

    Camera cam;
    double last_time;

    int win_w = WINDOW_W, win_h = WINDOW_H;   /* fallback if display query fails */

    static const EGLint config_attribs[] = {
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_DEPTH_SIZE, 16,
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_NONE
    };
    static const EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };

    pthread_mutex_init(&g_state.lock, NULL);

    /* ---- networking socket (handshake happens on the net thread) ---- */
    g_sock = net_udp_socket();
    if (g_sock < 0) { fprintf(stderr, "net_udp_socket() failed\n"); return 1; }
    memset(&g_server_addr, 0, sizeof(g_server_addr));
    g_server_addr.sin_family      = AF_INET;
    g_server_addr.sin_port        = htons(port);
    g_server_addr.sin_addr.s_addr = inet_addr(server_ip);
    printf("[net] Will connect to %s:%d\n", server_ip, port);

    /* ---- Screen setup ---- */
    if (screen_create_context(&screen_ctx, SCREEN_APPLICATION_CONTEXT) != 0) {
        perror("screen_create_context"); return 1;
    }

    /* Query the real display resolution -- SCREEN_PROPERTY_SIZE on a
     * DISPLAY object reports "the width and height, in pixels, of the
     * current video resolution" per QNX's own Screen API docs.
     * Enumerate via the context's SCREEN_PROPERTY_DISPLAY_COUNT /
     * SCREEN_PROPERTY_DISPLAYS first to get a display handle at all. */
    {
        int display_count = 0;
        screen_display_t displays[8];

        if (screen_get_context_property_iv(screen_ctx, SCREEN_PROPERTY_DISPLAY_COUNT,
                                           &display_count) != 0
            || display_count < 1) {
            fprintf(stderr, "[screen] Couldn't query display count -- "
                            "using fallback %dx%d\n", win_w, win_h);
        } else {
            if (display_count > 8) display_count = 8;
            if (screen_get_context_property_pv(screen_ctx, SCREEN_PROPERTY_DISPLAYS,
                                               (void **)displays) != 0) {
                fprintf(stderr, "[screen] Couldn't query display handles -- "
                                "using fallback %dx%d\n", win_w, win_h);
            } else {
                int size[2] = { 0, 0 };
                if (screen_get_display_property_iv(displays[0], SCREEN_PROPERTY_SIZE, size) != 0
                    || size[0] <= 0 || size[1] <= 0) {
                    fprintf(stderr, "[screen] Couldn't query display resolution -- "
                                    "using fallback %dx%d\n", win_w, win_h);
                } else {
                    win_w = size[0];
                    win_h = size[1];
                    printf("[screen] Detected display resolution: %dx%d\n", win_w, win_h);
                }
            }
        }
    }

    if (screen_create_window(&screen_win, screen_ctx) != 0) {
        perror("screen_create_window"); return 1;
    }
    if (screen_create_event(&screen_ev) != 0) {
        perror("screen_create_event"); return 1;
    }
    {
        int usage = SCREEN_USAGE_OPENGL_ES2;
        int format = SCREEN_FORMAT_RGBA8888;
        int size[2] = { win_w, win_h };
        int swap_interval = 1;
        screen_set_window_property_iv(screen_win, SCREEN_PROPERTY_USAGE, &usage);
        screen_set_window_property_iv(screen_win, SCREEN_PROPERTY_FORMAT, &format);
        screen_set_window_property_iv(screen_win, SCREEN_PROPERTY_SIZE, size);
        screen_set_window_property_iv(screen_win, SCREEN_PROPERTY_SWAP_INTERVAL, &swap_interval);
    }
    if (screen_create_window_buffers(screen_win, 2) != 0) {
        perror("screen_create_window_buffers"); return 1;
    }

    /* ---- EGL setup ---- */
    egl_disp = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (egl_disp == EGL_NO_DISPLAY) { fprintf(stderr, "eglGetDisplay() failed\n"); return 1; }
    if (eglInitialize(egl_disp, NULL, NULL) != EGL_TRUE) {
        fprintf(stderr, "eglInitialize() failed\n"); return 1;
    }
    eglBindAPI(EGL_OPENGL_ES_API);
    if (eglChooseConfig(egl_disp, config_attribs, &egl_config, 1, &num_config) != EGL_TRUE
        || num_config < 1) {
        fprintf(stderr, "eglChooseConfig() failed\n"); return 1;
    }
    egl_ctx = eglCreateContext(egl_disp, egl_config, EGL_NO_CONTEXT, ctx_attribs);
    if (egl_ctx == EGL_NO_CONTEXT) { fprintf(stderr, "eglCreateContext() failed\n"); return 1; }
    egl_surf = eglCreateWindowSurface(egl_disp, egl_config, screen_win, NULL);
    if (egl_surf == EGL_NO_SURFACE) { fprintf(stderr, "eglCreateWindowSurface() failed\n"); return 1; }
    if (eglMakeCurrent(egl_disp, egl_surf, egl_surf, egl_ctx) != EGL_TRUE) {
        fprintf(stderr, "eglMakeCurrent() failed\n"); return 1;
    }
    printf("[gl] GL_VERSION:  %s\n", (const char *)glGetString(GL_VERSION));
    printf("[gl] GL_RENDERER: %s\n", (const char *)glGetString(GL_RENDERER));

    /* ---- shader + static level geometry ---- */
    prog = build_shader_program(VERTEX_SHADER_SRC, FRAGMENT_SHADER_SRC, &mvp_loc);
    if (!prog) { fprintf(stderr, "Shader setup failed\n"); return 1; }
    pos_loc   = glGetAttribLocation(prog, "a_position");
    color_loc = glGetAttribLocation(prog, "a_color");

    /* Second program, level geometry (walls/floor/ceiling/platforms/
     * ramps) only -- see the comment above LEVEL_FRAGMENT_SHADER_SRC
     * for why this is separate from the simple program above rather
     * than one program handling everything. */
    level_prog = build_shader_program(LEVEL_VERTEX_SHADER_SRC, LEVEL_FRAGMENT_SHADER_SRC, &level_mvp_loc);
    if (!level_prog) { fprintf(stderr, "Level shader setup failed\n"); return 1; }
    level_pos_loc    = glGetAttribLocation(level_prog, "a_position");
    level_normal_loc = glGetAttribLocation(level_prog, "a_normal");
    level_color_loc  = glGetAttribLocation(level_prog, "a_color");

    /* Third program, real English text via the Public Pixel font
     * atlas -- see text_render.h for why this is its own pipeline
     * rather than extending either program above. mvp_loc is unused
     * here (the text vertex shader has no u_mvp uniform at all, see
     * TEXT_VERTEX_SHADER_SRC), the return value is just discarded. */
    {
        GLint unused_mvp_loc;
        text_prog = build_shader_program(TEXT_VERTEX_SHADER_SRC, TEXT_FRAGMENT_SHADER_SRC, &unused_mvp_loc);
    }
    if (!text_prog) { fprintf(stderr, "Text shader setup failed\n"); return 1; }
    text_pos_loc      = glGetAttribLocation(text_prog, "a_position");
    text_texcoord_loc = glGetAttribLocation(text_prog, "a_texcoord");
    text_color_loc    = glGetAttribLocation(text_prog, "a_color");
    text_tex_loc      = glGetUniformLocation(text_prog, "u_tex");

    /* Font atlas: single-channel (GL_ALPHA) texture, uploaded once at
     * startup from the embedded font_atlas.h -- never loaded from a
     * file on the target, same "everything is compiled in" approach
     * as the rest of this renderer. NEAREST filtering keeps the
     * pixel-font glyphs crisp instead of blurring them; CLAMP_TO_EDGE
     * avoids wrap-around bleeding at glyph cell edges. */
    glGenTextures(1, &font_texture);
    glBindTexture(GL_TEXTURE_2D, font_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, FONT_ATLAS_W, FONT_ATLAS_H_PX, 0,
                GL_ALPHA, GL_UNSIGNED_BYTE, font_atlas_pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenBuffers(1, &text_vbo);

    level_geo = build_level_geometry();
    level_vertex_count = level_geo.count;
    glGenBuffers(1, &level_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, level_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof(GeoVertex) * (size_t)level_geo.count),
                 level_geo.verts, GL_STATIC_DRAW);
    vertex_list_free(&level_geo);

    /* Dynamic buffers, rebuilt every frame -- entities move, HUD
     * values change. */
    glGenBuffers(1, &entity_vbo);
    glGenBuffers(1, &hud_vbo);

    glEnable(GL_DEPTH_TEST);
    /* Deliberately NOT calling glEnable(GL_CULL_FACE) -- see the
     * comment on FACES in level_geo.c for why. */

    /* ---- app state: starts in the menu; the network thread doesn't
     * start until Play Solo/Coop is confirmed there ---- */
    {
        int menu_selection  = MENU_OPTION_SOLO;
        int net_thread_started = 0;

        /* Camera starting position -- only meaningful once gameplay
         * actually begins, but harmless to set up now. */
        cam.x = (MAP_W * 0.5f);
        cam.z = (MAP_ROWS * 0.5f);
        cam.y = EYE_HEIGHT;
        cam.yaw = 0.0f;
        cam.pitch = 0.0f;
        cam.vel_y = 0.0f;
        cam.jumping = 0;

        last_time = portable_time();

        printf("[main] Entering menu. Up/Down to select, E to confirm.\n");
        while (g_running) {
            double now = portable_time();
            f32 dt = (f32)(now - last_time);
            last_time = now;
            if (dt > 0.1f) dt = 0.1f;

            for (;;) {
                int type = SCREEN_EVENT_NONE;
                if (screen_get_event(screen_ctx, screen_ev, 0) != 0) break;
                screen_get_event_property_iv(screen_ev, SCREEN_PROPERTY_TYPE, &type);
                if (type == SCREEN_EVENT_NONE) break;

                if (type == SCREEN_EVENT_KEYBOARD) handle_keyboard_event(screen_ev, &cam);
                else if (type == SCREEN_EVENT_POINTER) handle_pointer_event(screen_ev, &cam);
            }

            if (g_app_state == APP_STATE_MENU) {
                if (g_menu_nav_up_requested) {
                    g_menu_nav_up_requested = 0;
                    menu_selection = (menu_selection + MENU_OPTION_COUNT - 1) % MENU_OPTION_COUNT;
                }
                if (g_menu_nav_down_requested) {
                    g_menu_nav_down_requested = 0;
                    menu_selection = (menu_selection + 1) % MENU_OPTION_COUNT;
                }
                if (g_menu_confirm_requested) {
                    g_menu_confirm_requested = 0;
                    if (menu_selection == MENU_OPTION_QUIT) {
                        g_running = 0;
                    } else {
                        g_connect_mode = (menu_selection == MENU_OPTION_SOLO)
                            ? CONNECT_MODE_SOLO : CONNECT_MODE_COOP;

                        /* Fresh camera + input state entering gameplay --
                         * matters both for a first play and for a
                         * restart after returning from a previous
                         * session (see the return-to-menu handling
                         * below, which intentionally does NOT reset
                         * these, so this is the one place it happens). */
                        cam.x = (MAP_W * 0.5f);
                        cam.z = (MAP_ROWS * 0.5f);
                        cam.y = EYE_HEIGHT;
                        cam.yaw = 0.0f;
                        cam.pitch = 0.0f;
                        cam.vel_y = 0.0f;
                        cam.jumping = 0;
                        g_key_w = g_key_a = g_key_s = g_key_d = 0;
                        g_key_look_left = g_key_look_right = 0;
                        g_key_look_up = g_key_look_down = 0;
                        g_key_e = g_key_space = 0;
                        g_mouse_left_down = 0;
                        g_have_last_pointer = 0;

                        g_connected = 0;
                        g_my_id     = 0xFF;
                        g_net_thread_running = 1;
                        if (start_net_thread_with_scheduling(&net_tid) != 0) {
                            fprintf(stderr, "Continuing without networking\n");
                            g_net_thread_running = 0;
                        } else {
                            net_thread_started = 1;
                        }

                        g_app_state = APP_STATE_PLAYING;
                        printf("[main] Entering gameplay.\n");
                    }
                }

                if (g_running) {
                    TextVertexList menu_text;
                    VertexList menu_geo = build_menu_geometry(win_w, win_h, menu_selection, &menu_text);
                    Mat4 identity = mat4_identity();

                    glViewport(0, 0, win_w, win_h);
                    glClearColor(0.05f, 0.05f, 0.07f, 1.0f);
                    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

                    glUseProgram(prog);
                    glDisable(GL_DEPTH_TEST);
                    glUniformMatrix4fv(mvp_loc, 1, GL_FALSE, identity.m);
                    if (menu_geo.count > 0) {
                        glBindBuffer(GL_ARRAY_BUFFER, hud_vbo);
                        glBufferData(GL_ARRAY_BUFFER,
                                    (GLsizeiptr)(sizeof(GeoVertex) * (size_t)menu_geo.count),
                                    menu_geo.verts, GL_DYNAMIC_DRAW);
                        draw_vertex_list(hud_vbo, pos_loc, color_loc, menu_geo.count);
                    }
                    vertex_list_free(&menu_geo);

                    glUseProgram(text_prog);
                    draw_text(text_vbo, font_texture, text_pos_loc, text_texcoord_loc,
                             text_color_loc, text_tex_loc, &menu_text);
                    text_vlist_free(&menu_text);

                    glEnable(GL_DEPTH_TEST);

                    eglSwapBuffers(egl_disp, egl_surf);
                }
                continue;   /* skip the gameplay branch below this frame */
            }

            /* ---- APP_STATE_PLAYING ---- */
            if (g_return_to_menu_requested) {
                g_return_to_menu_requested = 0;

                g_net_thread_running = 0;
                if (net_thread_started) {
                    pthread_join(net_tid, NULL);
                    net_thread_started = 0;
                }

                /* Clear the shared snapshot so a stale zombie/player
                 * from the last session doesn't flash on screen for a
                 * frame before the next PktState arrives. */
                pthread_mutex_lock(&g_state.lock);
                g_state.player_count = 0;
                g_state.zombie_count = 0;
                g_state.pickup_count = 0;
                g_state.has_my_state = 0;
                pthread_mutex_unlock(&g_state.lock);

                g_app_state = APP_STATE_MENU;
                printf("[main] Returning to menu.\n");
                continue;   /* menu renders itself next iteration */
            }

            update_camera(&cam, dt);
            reconcile_camera(&cam);

            {
                Mat4 proj = mat4_perspective(70.0f * 3.14159265f / 180.0f,
                                            (f32)win_w / (f32)win_h, 0.1f, 100.0f);
                Mat4 view = mat4_view_from_yaw_pitch(cam.x, cam.y, cam.z, cam.yaw, cam.pitch);
                Mat4 mvp  = mat4_multiply(proj, view);
                Mat4 identity = mat4_identity();

                VertexList entity_geo = build_entity_geometry();
                VertexList hud_geo    = build_hud_geometry(win_w, win_h);

                glViewport(0, 0, win_w, win_h);
                glClearColor(0.02f, 0.02f, 0.03f, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

                /* 3D pass, level: separate program with the procedural
                 * brick pattern (see LEVEL_FRAGMENT_SHADER_SRC) --
                 * walls/floor/ceiling/platforms/ramps only. */
                glUseProgram(level_prog);
                glUniformMatrix4fv(level_mvp_loc, 1, GL_FALSE, mvp.m);
                draw_vertex_list_lit(level_vbo, level_pos_loc, level_normal_loc, level_color_loc,
                                     level_vertex_count);

                /* 3D pass, entities: back to the simple flat-color
                 * program -- a zombie or player marker shouldn't get
                 * the brick treatment. */
                glUseProgram(prog);
                glUniformMatrix4fv(mvp_loc, 1, GL_FALSE, mvp.m);
                if (entity_geo.count > 0) {
                    glBindBuffer(GL_ARRAY_BUFFER, entity_vbo);
                    glBufferData(GL_ARRAY_BUFFER,
                                (GLsizeiptr)(sizeof(GeoVertex) * (size_t)entity_geo.count),
                                entity_geo.verts, GL_DYNAMIC_DRAW);
                    draw_vertex_list(entity_vbo, pos_loc, color_loc, entity_geo.count);
                }
                vertex_list_free(&entity_geo);

                /* 2D HUD pass: identity MVP (hud_render.c emits NDC
                 * directly), depth test off so it always draws on top.
                 * Still the simple program -- HUD digits/crosshair stay
                 * flat-colored, no brick pattern. */
                glDisable(GL_DEPTH_TEST);
                glUniformMatrix4fv(mvp_loc, 1, GL_FALSE, identity.m);
                if (hud_geo.count > 0) {
                    glBindBuffer(GL_ARRAY_BUFFER, hud_vbo);
                    glBufferData(GL_ARRAY_BUFFER,
                                (GLsizeiptr)(sizeof(GeoVertex) * (size_t)hud_geo.count),
                                hud_geo.verts, GL_DYNAMIC_DRAW);
                    draw_vertex_list(hud_vbo, pos_loc, color_loc, hud_geo.count);
                }
                vertex_list_free(&hud_geo);
                glEnable(GL_DEPTH_TEST);   /* restore for next frame's 3D pass */
            }

            eglSwapBuffers(egl_disp, egl_surf);
        }

        g_running = 0;
        g_net_thread_running = 0;
        if (net_thread_started)
            pthread_join(net_tid, NULL);
        pthread_mutex_destroy(&g_state.lock);
    }

    eglMakeCurrent(egl_disp, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(egl_disp, egl_surf);
    eglDestroyContext(egl_disp, egl_ctx);
    eglTerminate(egl_disp);
    screen_destroy_window(screen_win);
    screen_destroy_context(screen_ctx);
    close(g_sock);
    return 0;
}
