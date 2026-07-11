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

#define WINDOW_W 1280   /* fallback resolution */
#define WINDOW_H 720

#define EYE_HEIGHT   1.4f    /* matches src/server.c's PLAYER_EYE_HEIGHT */
#define MOVE_SPEED   3.0f    /* world units/sec */
#define CLIMB_SPEED  1.0f    /* matches src/server.c's ZOMBIE_CLIMB_SPEED */
#define MOUSE_SENS   0.003f
#define LOOK_SPEED   2.0f    /* radians/sec, arrow-key look */
#define RECONCILE_LERP 0.08f /* per render frame (~60Hz) */
#define GRAVITY       9.8f   /* matches the Godot client's Player.gd, for
                              * a consistent feel between the two clients */
#define JUMP_VELOCITY 4.5f   /* matches the Godot client's Player.gd */

/* ------------------------------------------------------------------ */
/* Shared scalar state -- plain volatile, matching the rest of this    */
/* project's cross-thread convention for simple flags/values.          */
/* ------------------------------------------------------------------ */
static volatile int g_running   = 1;
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
static volatile int g_mouse_left_down = 0;
static volatile f32 g_cam_yaw = 0.0f, g_cam_pitch = 0.0f;

static int g_have_last_pointer = 0;
static int g_last_pointer_x = 0, g_last_pointer_y = 0;

static int                g_sock = -1;
static struct sockaddr_in g_server_addr;

/* ------------------------------------------------------------------ */
/* Shared entity (mutex)                                               */
/* ------------------------------------------------------------------ */
typedef struct {
    u8  id;
    int alive;
    f32 x, y;
} RenderPlayer;

typedef struct {
    u8  id;
    int alive;
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
    int          has_my_state;   /* false until the first PktState with player entry arrives */
    f32          my_x, my_y;     /* server's authoritative position for the player */
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
    while (!g_connected && g_running && portable_time() < deadline) {
        PktConnect pkt;
        double wait_until;

        memset(&pkt, 0, sizeof(pkt));
        pkt.hdr.type = PKT_CONNECT;
        pkt.hdr.tick = local_tick++;
        pkt.mode     = CONNECT_MODE_COOP;
        sendto(g_sock, &pkt, sizeof(pkt), 0,
               (struct sockaddr *)&g_server_addr, sizeof(g_server_addr));

        wait_until = portable_time() + 0.5;
        while (g_running && !g_connected && portable_time() < wait_until) {
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

    while (g_running) {
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
    int render_prio = max_fifo;
    int net_prio    = (max_fifo + min_fifo) / 2;

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
    if (rc != 0)
        fprintf(stderr, "[sched] pthread_create (net thread) failed: %d\n", rc);
    else
        printf("[sched] net thread: SCHED_FIFO priority %d\n", net_prio);
    pthread_attr_destroy(&net_attr);
    return rc;
}

/* ------------------------------------------------------------------ */
/* GLES2 shader pipeline                                               */
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

static GLuint build_shader_program(GLint *out_mvp_loc, GLint *out_pos_loc, GLint *out_color_loc)
{
    GLuint vs, fs, prog;
    GLint  ok;

    vs = compile_shader(GL_VERTEX_SHADER, VERTEX_SHADER_SRC);
    fs = compile_shader(GL_FRAGMENT_SHADER, FRAGMENT_SHADER_SRC);
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

    *out_pos_loc   = glGetAttribLocation(prog, "a_position");
    *out_color_loc = glGetAttribLocation(prog, "a_color");
    *out_mvp_loc   = glGetUniformLocation(prog, "u_mvp");
    return prog;
}

static void draw_vertex_list(GLuint vbo, GLint pos_loc, GLint color_loc, int vertex_count)
{
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glVertexAttribPointer((GLuint)pos_loc, 3, GL_FLOAT, GL_FALSE,
                          sizeof(GeoVertex), (const void *)0);
    glEnableVertexAttribArray((GLuint)pos_loc);
    glVertexAttribPointer((GLuint)color_loc, 3, GL_FLOAT, GL_FALSE,
                          sizeof(GeoVertex), (const void *)(3 * sizeof(f32)));
    glEnableVertexAttribArray((GLuint)color_loc);
    glDrawArrays(GL_TRIANGLES, 0, vertex_count);
}

/* ------------------------------------------------------------------ */
/* Camera + input state                                                */
/* ------------------------------------------------------------------ */
typedef struct {
    f32 x, y, z;
    f32 yaw, pitch;
    f32 vel_y;      /* vertical velocity, for jumping, 0 while grounded */
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
    else if (lower == 'e') g_key_e = down;   /* fallback shoot */
    else if (sym == ' ') {   /* jump */
        if (down && !g_key_space) g_jump_requested = 1;   /* rising edge only */
        g_key_space = down;
    }
    else if (sym == KEYCODE_LEFT)  g_key_look_left  = down;
    else if (sym == KEYCODE_RIGHT) g_key_look_right = down;
    else if (sym == KEYCODE_UP)    g_key_look_up    = down;
    else if (sym == KEYCODE_DOWN)  g_key_look_down  = down;
    else if (sym == KEYCODE_ESCAPE && down) g_running = 0;
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
 * collision via map_is_wall() and ramp/platform height easing. 
 * Also mirrors the resulting look angle into the shared g_cam_yaw/g_cam_pitch 
 * so the network thread can send
 * real look angles.
 */
static void update_camera(Camera *cam, f32 dt)
{
    f32 fx, fz, rx, rz;
    f32 mvx = 0.0f, mvz = 0.0f;
    f32 len;
    f32 new_x, new_z, target_y;
    int tile_mx, tile_my;

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

    tile_mx = (int)cam->x;
    tile_my = (int)cam->z;
    target_y = level_height_for_tile(tile_mx, tile_my) + EYE_HEIGHT;

    if (!cam->jumping) {
        f32 climb_step = CLIMB_SPEED * dt;
        if (cam->y < target_y) {
            cam->y += climb_step;
            if (cam->y > target_y) cam->y = target_y;
        } else if (cam->y > target_y) {
            cam->y -= climb_step;
            if (cam->y < target_y) cam->y = target_y;
        }

        if (g_jump_requested) {
            g_jump_requested = 0;
            cam->vel_y  = JUMP_VELOCITY;
            cam->jumping = 1;
        }
    } else {
        cam->vel_y -= GRAVITY * dt;
        cam->y     += cam->vel_y * dt;

        if (cam->y <= target_y) {
            cam->y      = target_y;   
            cam->vel_y  = 0.0f;
            cam->jumping = 0;
        }
    }

    g_cam_yaw   = atan2f(fz, fx);
    g_cam_pitch = cam->pitch;
}

/* Pulls the camera's (x, z) toward the server's authoritative position
 * for this player
 */
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

/* Builds a fresh vertex list for every zombie/other-player */
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
                 0.9f, 0.75f, 0.1f);   /* yellow */
    }
    for (i = 0; i < zombie_count; i++) {
        if (!zombies[i].alive) continue;
        push_box(&vl, zombies[i].x, zombies[i].z + 0.9f, zombies[i].y,
                 0.35f, 0.9f, 0.35f,
                 0.25f, 0.55f, 0.2f);   /* green */
    }
    for (i = 0; i < pickup_count; i++) {
        if (!pickups[i].active) continue;   /* on cooldown, hidden */
        if (pickups[i].type == PICKUP_AMMO) {
            push_box(&vl, pickups[i].x, 0.9f, pickups[i].y,
                     0.175f, 0.175f, 0.175f,
                     0.9f, 0.7f, 0.15f);    /* brass */
        } else {
            push_box(&vl, pickups[i].x, 0.9f, pickups[i].y,
                     0.175f, 0.175f, 0.175f,
                     0.85f, 0.15f, 0.2f);   /* red */
        }
    }

    return vl;
}

/* Builds the HUD overlay for the current frame: crosshair + health/
 * ammo/wave/zombies-left as 7-segment numbers. */
static VertexList build_hud_geometry(int win_w, int win_h)
{
    VertexList vl;
    u8  health, ammo, wave;
    int zombie_count;

    memset(&vl, 0, sizeof(vl));

    pthread_mutex_lock(&g_state.lock);
    health       = g_state.my_health;
    ammo         = g_state.my_ammo;
    wave         = g_state.wave;
    zombie_count = g_state.zombie_count;
    pthread_mutex_unlock(&g_state.lock);

    hud_push_crosshair(&vl, win_w, win_h, 1.0f, 1.0f, 1.0f);

    /* bottom-left: health */
    hud_push_number(&vl, (int)health, 24.0f, (f32)win_h - 56.0f, 20.0f, 32.0f,
                    win_w, win_h, 0.2f, 0.9f, 0.2f);
    /* bottom-right: ammo */
    hud_push_number(&vl, (int)ammo, (f32)win_w - 140.0f, (f32)win_h - 56.0f, 20.0f, 32.0f,
                    win_w, win_h, 0.9f, 0.7f, 0.15f);
    /* top-right: wave */
    hud_push_number(&vl, (int)wave, (f32)win_w - 140.0f, 24.0f, 20.0f, 32.0f,
                    win_w, win_h, 0.8f, 0.8f, 0.9f);
    /* top-left: zombies remaining */
    hud_push_number(&vl, zombie_count, 24.0f, 24.0f, 20.0f, 32.0f,
                    win_w, win_h, 0.9f, 0.3f, 0.3f);

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

    GLuint  prog;
    GLint   pos_loc, color_loc, mvp_loc;
    GLuint  level_vbo, entity_vbo, hud_vbo;
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

    /* Query the real display resolution using SCREEN_PROPERTY_SIZE used on a
     * display object reports the width and height, in pixels, of the
     * current video resolution.
     */
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
    prog = build_shader_program(&mvp_loc, &pos_loc, &color_loc);
    if (!prog) { fprintf(stderr, "Shader setup failed\n"); return 1; }

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
    /* Deliberately NOT calling glEnable(GL_CULL_FACE) */

    /* ---- RTOS scheduling + network thread ---- */
    if (start_net_thread_with_scheduling(&net_tid) != 0)
        fprintf(stderr, "Continuing without networking\n");

    /* ---- camera starting position: center of the map, looking down
     * -Z, standing on the floor ---- */
    cam.x = (MAP_W * 0.5f);
    cam.z = (MAP_ROWS * 0.5f);
    cam.y = EYE_HEIGHT;
    cam.yaw = 0.0f;
    cam.pitch = 0.0f;
    cam.vel_y = 0.0f;
    cam.jumping = 0;

    last_time = portable_time();

    printf("[main] Entering render loop. WASD/mouse or arrows to move+look,\n"
           "       Space to jump, left mouse button or E to shoot, ESC to quit.\n");
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

            glUseProgram(prog);

            /* 3D pass: level (static) + entities (rebuilt this frame) */
            glUniformMatrix4fv(mvp_loc, 1, GL_FALSE, mvp.m);
            draw_vertex_list(level_vbo, pos_loc, color_loc, level_vertex_count);

            if (entity_geo.count > 0) {
                glBindBuffer(GL_ARRAY_BUFFER, entity_vbo);
                glBufferData(GL_ARRAY_BUFFER,
                            (GLsizeiptr)(sizeof(GeoVertex) * (size_t)entity_geo.count),
                            entity_geo.verts, GL_DYNAMIC_DRAW);
                draw_vertex_list(entity_vbo, pos_loc, color_loc, entity_geo.count);
            }
            vertex_list_free(&entity_geo);

            /* 2D HUD pass: identity MVP (hud_render.c emits NDC
             * directly), depth test off so it always draws on top. */
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
    pthread_join(net_tid, NULL);
    pthread_mutex_destroy(&g_state.lock);

    eglMakeCurrent(egl_disp, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(egl_disp, egl_surf);
    eglDestroyContext(egl_disp, egl_ctx);
    eglTerminate(egl_disp);
    screen_destroy_window(screen_win);
    screen_destroy_context(screen_ctx);
    close(g_sock);
    return 0;
}
