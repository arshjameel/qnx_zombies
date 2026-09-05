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

#define WINDOW_W 1280   /* fallback size */
#define WINDOW_H 720

#define EYE_HEIGHT   1.4f    
#define MOVE_SPEED   3.0f  

#define APP_STATE_MENU    0
#define APP_STATE_PLAYING 1
#define APP_STATE_WON     2
#define MENU_OPTION_SOLO   0
#define MENU_OPTION_COOP   1
#define MENU_OPTION_QUIT   2
#define MENU_OPTION_COUNT  3
#define WON_OPTION_QUIT_MENU 0
#define WON_OPTION_QUIT_GAME 1
#define WON_OPTION_COUNT  2
#define WAVE_COUNT 4
#define CLIMB_SPEED_UP   3.0f
#define CLIMB_SPEED_DOWN 8.0f
#define MOUSE_SENS   0.003f
#define LOOK_SPEED   2.0f    
#define RECONCILE_LERP 0.08f 
#define GRAVITY       14.0f  
#define JUMP_VELOCITY 5.0f 
#define PLATFORM_ELEVATED_THRESHOLD (LEVEL_PLATFORM_HEIGHT * 0.6f)
#define DAMAGE_FLASH_DECAY_TIME 0.5f   

static volatile int g_running   = 1;
static volatile int g_net_thread_running = 0;
static volatile int g_return_to_menu_requested = 0;   
static volatile int g_app_state = APP_STATE_MENU;   
static volatile int g_connected = 0;
static volatile u8  g_my_id     = 0xFF;
static volatile f32 g_spawn_x, g_spawn_y, g_spawn_angle;

static volatile int g_key_w = 0, g_key_a = 0, g_key_s = 0, g_key_d = 0;
static volatile int g_key_look_left = 0, g_key_look_right = 0;
static volatile int g_key_look_up = 0, g_key_look_down = 0;
static volatile int g_key_e = 0;   
static volatile int g_key_enter = 0;   
static volatile int g_key_space = 0;
static volatile int g_jump_requested = 0;   

static volatile int g_menu_nav_up_requested   = 0;
static volatile int g_menu_nav_down_requested = 0;
static volatile int g_menu_confirm_requested  = 0;
static volatile int g_connect_mode = CONNECT_MODE_COOP;   
static volatile int g_reset_requested = 0;
static volatile f32 g_damage_flash = 0.0f;   
static volatile int g_mouse_left_down = 0;
static volatile f32 g_cam_yaw = 0.0f, g_cam_pitch = 0.0f;

static int g_have_last_pointer = 0;
static int g_last_pointer_x = 0, g_last_pointer_y = 0;

static int                g_sock = -1;
static struct sockaddr_in g_server_addr;

typedef struct {
    u8  id;
    int alive;
    f32 x, y;
} RenderPlayer;

typedef struct {
    u8  id;
    int alive;
    u8  type;
    f32 x, y, z;
} RenderZombie;

typedef struct {
    u8  id;
    u8  type;      
    int active;
    f32 x, y;
} RenderPickup;

#define FEED_MAX_LINES 4
#define FEED_TTL       5.0f

typedef struct {
    char text[80];
    f32  r, g, b;
    f32  ttl;
} FeedEntry;

typedef struct {
    pthread_mutex_t lock;
    RenderPlayer players[NET_MAX_PLAYERS];
    int          player_count;
    RenderZombie zombies[MAX_ZOMBIES];
    int          zombie_count;
    RenderPickup pickups[MAX_PICKUP_SPAWNS];
    int          pickup_count;
    u8           wave;
    int          has_my_state;   
    f32          my_x, my_y;
    u8           my_health;
    u8           my_ammo;
    FeedEntry    feed[FEED_MAX_LINES];  
    int          feed_count;
} SharedGameState;

static SharedGameState g_state;

static void push_feed_entry(const char *text, f32 r, f32 g, f32 b)
{
    pthread_mutex_lock(&g_state.lock);
    if (g_state.feed_count >= FEED_MAX_LINES) {
        int i;
        for (i = 1; i < FEED_MAX_LINES; i++) g_state.feed[i - 1] = g_state.feed[i];
        g_state.feed_count = FEED_MAX_LINES - 1;
    }
    {
        FeedEntry *e = &g_state.feed[g_state.feed_count];
        strncpy(e->text, text, sizeof(e->text) - 1);
        e->text[sizeof(e->text) - 1] = '\0';
        e->r = r; e->g = g; e->b = b;
        e->ttl = FEED_TTL;
        g_state.feed_count++;
    }
    pthread_mutex_unlock(&g_state.lock);
}

static int get_feed_snapshot(FeedEntry *out, f32 dt)
{
    int i, w, n;
    pthread_mutex_lock(&g_state.lock);
    for (i = 0; i < g_state.feed_count; i++) g_state.feed[i].ttl -= dt;
    w = 0;
    for (i = 0; i < g_state.feed_count; i++) {
        if (g_state.feed[i].ttl > 0.0f) {
            if (w != i) g_state.feed[w] = g_state.feed[i];
            w++;
        }
    }
    g_state.feed_count = w;
    n = g_state.feed_count;
    for (i = 0; i < n; i++) out[i] = g_state.feed[i];
    pthread_mutex_unlock(&g_state.lock);
    return n;
}

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

        if (g_reset_requested) {
            PktHeader reset_hdr;
            g_reset_requested = 0;
            memset(&reset_hdr, 0, sizeof(reset_hdr));
            reset_hdr.type      = PKT_RESET_SESSION;
            reset_hdr.player_id = g_my_id;
            reset_hdr.tick      = local_tick++;
            sendto(g_sock, &reset_hdr, sizeof(reset_hdr), 0,
                  (struct sockaddr *)&g_server_addr, sizeof(g_server_addr));
        }

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
                        continue;  
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
            } else if (hdr->type == PKT_HIT && n >= (ssize_t)sizeof(PktHit)) {
                PktHit *hit = (PktHit *)buf;
                char msg[80];
                f32 r, g, b;

                if (hit->victim_type == ENTITY_ZOMBIE) {
                    if (hit->headshot) {
                        snprintf(msg, sizeof(msg), "[HEADSHOT] PLAYER %d HIT ZOMBIE %d",
                                hit->attacker_id, hit->victim_id);
                        r = 0.95f; g = 0.6f; b = 0.1f;    /* orange */
                    } else {
                        snprintf(msg, sizeof(msg), "PLAYER %d HIT ZOMBIE %d",
                                hit->attacker_id, hit->victim_id);
                        r = 0.55f; g = 0.9f; b = 0.55f;   /* light green */
                    }
                } else {
                    if (hit->attacker_id == ATTACKER_ZOMBIE) {
                        snprintf(msg, sizeof(msg), "ZOMBIE MAULED PLAYER %d", hit->victim_id);
                        r = 0.9f; g = 0.2f; b = 0.2f;     /* red */
                        if (hit->victim_id == g_my_id) g_damage_flash = 1.0f;
                    } else {
                        snprintf(msg, sizeof(msg), "PLAYER %d HIT PLAYER %d",
                                hit->attacker_id, hit->victim_id);
                        r = 0.9f; g = 0.85f; b = 0.25f;   /* yellow */
                    }
                }
                push_feed_entry(msg, r, g, b);
            }
        }
        portable_sleep_ms(50);
    }

    if (g_connected) {
        PktHeader disc;
        memset(&disc, 0, sizeof(disc));
        disc.type      = PKT_DISCONNECT;
        disc.player_id = g_my_id;
        sendto(g_sock, &disc, sizeof(disc), 0,
              (struct sockaddr *)&g_server_addr, sizeof(g_server_addr));
    }

    return NULL;
}

static void teardown_and_return_to_menu(pthread_t net_tid, int *net_thread_started)
{
    g_net_thread_running = 0;
    if (*net_thread_started) {
        pthread_join(net_tid, NULL);
        *net_thread_started = 0;
    }

    pthread_mutex_lock(&g_state.lock);
    g_state.player_count = 0;
    g_state.zombie_count = 0;
    g_state.pickup_count = 0;
    g_state.has_my_state = 0;
    g_state.feed_count   = 0;
    pthread_mutex_unlock(&g_state.lock);

    g_app_state = APP_STATE_MENU;
    printf("[main] Returning to menu.\n");
}

static int start_net_thread_with_scheduling(pthread_t *out_tid)
{
    int min_fifo = sched_get_priority_min(SCHED_FIFO);
    int max_fifo = sched_get_priority_max(SCHED_FIFO);
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
    "    float brick_id = floor((u + row_offset) / brick_w) + row * 13.0;\n"
    "    float tint = 0.92 + 0.08 * fract(sin(brick_id * 12.9898) * 43758.5453);\n"
    "    vec3 brick_color = v_color * tint * (1.0 - mortar_line * 0.55);\n"
    "    vec3 final_color = mix(v_color, brick_color, wallness);\n"
    "    gl_FragColor = vec4(final_color, 1.0);\n"
    "}\n";

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
    "    float a = texture2D(u_tex, v_texcoord).a;\n"
    "    if (a < 0.5) discard;\n"
    "    gl_FragColor = vec4(v_color, 1.0);\n"
    "}\n";

static const char *VIGNETTE_VERTEX_SHADER_SRC =
    "attribute vec2 a_position;\n"
    "varying vec2 v_ndc;\n"
    "void main() {\n"
    "    gl_Position = vec4(a_position, 0.0, 1.0);\n"
    "    v_ndc = a_position;\n"
    "}\n";

static const char *VIGNETTE_FRAGMENT_SHADER_SRC =
    "precision mediump float;\n"
    "uniform float u_intensity;\n"
    "varying vec2 v_ndc;\n"
    "void main() {\n"
    "    float dist = length(v_ndc);\n"
    "    float vignette = smoothstep(0.3, 1.1, dist);\n"
    "    float alpha = vignette * u_intensity * 0.65;\n"
    "    gl_FragColor = vec4(0.8, 0.05, 0.05, alpha);\n"
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

typedef struct {
    f32 x, y, z;
    f32 yaw, pitch;
    f32 vel_y;      
    int jumping;    
} Camera;

static void handle_keyboard_event(screen_event_t ev, Camera *cam)
{
    int flags = 0, sym = 0, down, lower;
    (void)cam;
    screen_get_event_property_iv(ev, SCREEN_PROPERTY_FLAGS, &flags);
    screen_get_event_property_iv(ev, SCREEN_PROPERTY_SYM, &sym);
    down  = (flags & KEY_DOWN) ? 1 : 0;
    lower = sym | 0x20;   

    if (lower == 'w') g_key_w = down;
    else if (lower == 'a') g_key_a = down;
    else if (lower == 's') g_key_s = down;
    else if (lower == 'd') g_key_d = down;
    else if (lower == 'e') {
        g_key_e = down;   
    }
    else if (sym == ' ') {   
        if (down && !g_key_space) g_jump_requested = 1;
        g_key_space = down;
    }
    else if (sym == KEYCODE_LEFT)  g_key_look_left  = down;
    else if (sym == KEYCODE_RIGHT) g_key_look_right = down;
    else if (sym == KEYCODE_UP) {
        if (down && !g_key_look_up) g_menu_nav_up_requested = 1;   
        g_key_look_up = down;
    }
    else if (sym == KEYCODE_DOWN) {
        if (down && !g_key_look_down) g_menu_nav_down_requested = 1;
        g_key_look_down = down;
    }
    else if (sym == KEYCODE_RETURN || sym == KEYCODE_KP_ENTER) {
        if (down && !g_key_enter) g_menu_confirm_requested = 1;
        g_key_enter = down;
    }
    else if (sym == KEYCODE_ESCAPE && down) {
        if (g_app_state == APP_STATE_PLAYING) g_return_to_menu_requested = 1;
        else g_running = 0;   
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
            cam->y = target_y;
        } else {
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

static void reconcile_camera(Camera *cam)
{
    int has_state;
    f32 server_x, server_y;

    pthread_mutex_lock(&g_state.lock);
    has_state = g_state.has_my_state;
    server_x  = g_state.my_x;
    server_y  = g_state.my_y;
    pthread_mutex_unlock(&g_state.lock);

    if (!has_state) return;   

    cam->x += (server_x - cam->x) * RECONCILE_LERP;
    cam->z += (server_y - cam->z) * RECONCILE_LERP;
}

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
                 0.9f, 0.75f, 0.1f);   /* yellow teammate */
    }
    for (i = 0; i < zombie_count; i++) {
        f32 scale, r, g, b, half_h, half_r;
        if (!zombies[i].alive) continue;

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
        if (!pickups[i].active) continue;   
        if (pickups[i].type == PICKUP_AMMO) {
            push_box(&vl, pickups[i].x, 0.9f, pickups[i].y,
                     0.175f, 0.175f, 0.175f,
                     0.9f, 0.7f, 0.15f);    /* amber */
        } else {
            push_box(&vl, pickups[i].x, 0.9f, pickups[i].y,
                     0.175f, 0.175f, 0.175f,
                     0.85f, 0.15f, 0.2f);   /* red */
        }
    }

    return vl;
}

#define PLAYER_MAX_HEALTH 100
#define PLAYER_MAX_AMMO   60

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
    return x - px;
}

static VertexList build_hud_geometry(int win_w, int win_h, f32 dt, TextVertexList *out_text)
{
    VertexList vl;
    u8  health, ammo, wave;
    int zombie_count;
    const f32 digit_w = 20.0f, digit_h = 32.0f;
    const f32 label_w = 14.0f, label_h = 18.0f;
    f32 ammo_width, ammo_label_w;
    FeedEntry feed[FEED_MAX_LINES];
    int feed_n, i;

    memset(&vl, 0, sizeof(vl));
    memset(out_text, 0, sizeof(*out_text));

    pthread_mutex_lock(&g_state.lock);
    health       = g_state.my_health;
    ammo         = g_state.my_ammo;
    wave         = g_state.wave;
    zombie_count = g_state.zombie_count;
    pthread_mutex_unlock(&g_state.lock);

    hud_push_crosshair(&vl, win_w, win_h, 1.0f, 1.0f, 1.0f);

    text_push_string(out_text, "HP", 24.0f, (f32)win_h - 56.0f - label_h - 4.0f,
                     label_w, label_h, win_w, win_h, 0.6f, 0.9f, 0.6f);
    push_stat_ratio(&vl, (int)health, PLAYER_MAX_HEALTH, 24.0f, (f32)win_h - 56.0f,
                    digit_w, digit_h, win_w, win_h, 0.2f, 0.9f, 0.2f);

    {
        VertexList measure;
        memset(&measure, 0, sizeof(measure));
        ammo_width = push_stat_ratio(&measure, (int)ammo, PLAYER_MAX_AMMO, 0.0f, 0.0f,
                                     digit_w, digit_h, win_w, win_h, 0, 0, 0);
        vertex_list_free(&measure);
    }
    ammo_label_w = text_string_width("AMMO", label_w);
    text_push_string(out_text, "AMMO", (f32)win_w - 24.0f - ammo_label_w,
                     (f32)win_h - 56.0f - label_h - 4.0f,
                     label_w, label_h, win_w, win_h, 0.95f, 0.8f, 0.4f);
    push_stat_ratio(&vl, (int)ammo, PLAYER_MAX_AMMO,
                    (f32)win_w - 24.0f - ammo_width, (f32)win_h - 56.0f,
                    digit_w, digit_h, win_w, win_h, 0.9f, 0.7f, 0.15f);
    text_push_string(out_text, "WAVE", (f32)win_w - 140.0f, 24.0f,
                     label_w, label_h, win_w, win_h, 0.8f, 0.8f, 0.9f);
    hud_push_number(&vl, (int)wave, (f32)win_w - 140.0f, 24.0f + label_h + 4.0f, 20.0f, 32.0f,
                    win_w, win_h, 0.8f, 0.8f, 0.9f);
    text_push_string(out_text, "ZOMBIES", 24.0f, 24.0f,
                     label_w, label_h, win_w, win_h, 0.95f, 0.5f, 0.5f);
    hud_push_number(&vl, zombie_count, 24.0f, 24.0f + label_h + 4.0f, 20.0f, 32.0f,
                    win_w, win_h, 0.9f, 0.3f, 0.3f);

    {
        const f32 feed_char_w = 12.0f, feed_char_h = 16.0f, feed_line_h = 22.0f;
        f32 feed_top = 24.0f + label_h + 4.0f + 32.0f + 16.0f;   

        feed_n = get_feed_snapshot(feed, dt);
        for (i = 0; i < feed_n; i++) {
            f32 w = text_string_width(feed[i].text, feed_char_w);
            text_push_string(out_text, feed[i].text, (f32)win_w - 24.0f - w,
                             feed_top + (f32)i * feed_line_h,
                             feed_char_w, feed_char_h, win_w, win_h,
                             feed[i].r, feed[i].g, feed[i].b);
        }
    }

    return vl;
}

static VertexList build_menu_geometry(int win_w, int win_h, int selection, TextVertexList *out_text)
{
    VertexList vl;
    const f32 box_w = 320.0f, box_h = 70.0f, spacing = 100.0f;
    const f32 base_colors[MENU_OPTION_COUNT][3] = {
        { 0.25f, 0.70f, 0.30f },   /* Solo */
        { 0.25f, 0.45f, 0.85f },   /* Coop */
        { 0.65f, 0.20f, 0.20f },   /* Quit */
    };
    static const char *labels[MENU_OPTION_COUNT] = { "SOLO", "COOP", "QUIT" };
    f32 first_top = (f32)win_h * 0.35f;
    f32 cx = (f32)win_w * 0.5f;
    int i;

    memset(&vl, 0, sizeof(vl));
    memset(out_text, 0, sizeof(*out_text));

    {
        const char *title = "QNX ZOMBIES";
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
        f32 dim    = (i == selection) ? 1.0f : 0.45f;   
        f32 r = base_colors[i][0] * dim;
        f32 g = base_colors[i][1] * dim;
        f32 b = base_colors[i][2] * dim;
        f32 label_char_w = 24.0f, label_char_h = 34.0f;
        f32 label_w = text_string_width(labels[i], label_char_w);

        hud_push_quad(&vl, left, top, right, bottom, win_w, win_h, r, g, b);
        
        text_push_string(out_text, labels[i], cx - label_w * 0.5f,
                         top + (box_h - label_char_h) * 0.5f,
                         label_char_w, label_char_h, win_w, win_h, 1.0f, 1.0f, 1.0f);
    }

    return vl;
}

static VertexList build_won_geometry(int win_w, int win_h, int selection, TextVertexList *out_text)
{
    VertexList vl;
    const f32 box_w = 340.0f, box_h = 70.0f, spacing = 100.0f;
    const f32 base_colors[WON_OPTION_COUNT][3] = {
        { 0.55f, 0.30f, 0.65f },   /* Quit to menu */
        { 0.65f, 0.20f, 0.20f },   /* Quit game */
    };
    static const char *labels[WON_OPTION_COUNT] = { "QUIT TO MENU", "QUIT GAME" };
    f32 first_top = (f32)win_h * 0.42f;
    f32 cx = (f32)win_w * 0.5f;
    int i;

    memset(&vl, 0, sizeof(vl));
    memset(out_text, 0, sizeof(*out_text));

    {
        const char *title = "THANK YOU FOR PLAYING";
        f32 title_char_w = 22.0f, title_char_h = 32.0f;
        f32 title_w = text_string_width(title, title_char_w);
        text_push_string(out_text, title, cx - title_w * 0.5f, first_top - 110.0f,
                         title_char_w, title_char_h, win_w, win_h, 0.95f, 0.85f, 0.3f);
    }
    {
        const char *sub = "QNX ZOMBIES";
        f32 sub_char_w = 26.0f, sub_char_h = 36.0f;
        f32 sub_w = text_string_width(sub, sub_char_w);
        text_push_string(out_text, sub, cx - sub_w * 0.5f, first_top - 65.0f,
                         sub_char_w, sub_char_h, win_w, win_h, 0.95f, 0.85f, 0.3f);
    }

    for (i = 0; i < WON_OPTION_COUNT; i++) {
        f32 top    = first_top + (f32)i * spacing;
        f32 bottom = top + box_h;
        f32 left   = cx - box_w * 0.5f;
        f32 right  = cx + box_w * 0.5f;
        f32 dim    = (i == selection) ? 1.0f : 0.45f;
        f32 r = base_colors[i][0] * dim;
        f32 g = base_colors[i][1] * dim;
        f32 b = base_colors[i][2] * dim;
        f32 label_char_w = 22.0f, label_char_h = 30.0f;
        f32 label_w = text_string_width(labels[i], label_char_w);

        hud_push_quad(&vl, left, top, right, bottom, win_w, win_h, r, g, b);
        text_push_string(out_text, labels[i], cx - label_w * 0.5f,
                         top + (box_h - label_char_h) * 0.5f,
                         label_char_w, label_char_h, win_w, win_h, 1.0f, 1.0f, 1.0f);
    }

    return vl;
}

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

    GLuint  prog, level_prog, text_prog, vignette_prog;
    GLint   pos_loc, color_loc, mvp_loc;
    GLint   level_pos_loc, level_normal_loc, level_color_loc, level_mvp_loc;
    GLint   text_pos_loc, text_texcoord_loc, text_color_loc, text_tex_loc;
    GLint   vignette_pos_loc, vignette_intensity_loc;
    GLuint  level_vbo, entity_vbo, hud_vbo, text_vbo, vignette_vbo;
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

    /* networking socket */
    g_sock = net_udp_socket();
    if (g_sock < 0) { fprintf(stderr, "net_udp_socket() failed\n"); return 1; }
    memset(&g_server_addr, 0, sizeof(g_server_addr));
    g_server_addr.sin_family      = AF_INET;
    g_server_addr.sin_port        = htons(port);
    g_server_addr.sin_addr.s_addr = inet_addr(server_ip);
    printf("[net] Will connect to %s:%d\n", server_ip, port);

    /* screen setup */
    if (screen_create_context(&screen_ctx, SCREEN_APPLICATION_CONTEXT) != 0) {
        perror("screen_create_context"); return 1;
    }

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

    /* EGL setup */
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

    /* shader and static level geometry */
    prog = build_shader_program(VERTEX_SHADER_SRC, FRAGMENT_SHADER_SRC, &mvp_loc);
    if (!prog) { fprintf(stderr, "Shader setup failed\n"); return 1; }
    pos_loc   = glGetAttribLocation(prog, "a_position");
    color_loc = glGetAttribLocation(prog, "a_color");

    level_prog = build_shader_program(LEVEL_VERTEX_SHADER_SRC, LEVEL_FRAGMENT_SHADER_SRC, &level_mvp_loc);
    if (!level_prog) { fprintf(stderr, "Level shader setup failed\n"); return 1; }
    level_pos_loc    = glGetAttribLocation(level_prog, "a_position");
    level_normal_loc = glGetAttribLocation(level_prog, "a_normal");
    level_color_loc  = glGetAttribLocation(level_prog, "a_color");

    {
        GLint unused_mvp_loc;
        text_prog = build_shader_program(TEXT_VERTEX_SHADER_SRC, TEXT_FRAGMENT_SHADER_SRC, &unused_mvp_loc);
    }
    if (!text_prog) { fprintf(stderr, "Text shader setup failed\n"); return 1; }
    text_pos_loc      = glGetAttribLocation(text_prog, "a_position");
    text_texcoord_loc = glGetAttribLocation(text_prog, "a_texcoord");
    text_color_loc    = glGetAttribLocation(text_prog, "a_color");
    text_tex_loc      = glGetUniformLocation(text_prog, "u_tex");

    glGenTextures(1, &font_texture);
    glBindTexture(GL_TEXTURE_2D, font_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, FONT_ATLAS_W, FONT_ATLAS_H_PX, 0,
                GL_ALPHA, GL_UNSIGNED_BYTE, font_atlas_pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenBuffers(1, &text_vbo);

    {
        GLint unused_mvp_loc;
        vignette_prog = build_shader_program(VIGNETTE_VERTEX_SHADER_SRC, VIGNETTE_FRAGMENT_SHADER_SRC,
                                             &unused_mvp_loc);
    }
    if (!vignette_prog) { fprintf(stderr, "Vignette shader setup failed\n"); return 1; }
    vignette_pos_loc       = glGetAttribLocation(vignette_prog, "a_position");
    vignette_intensity_loc = glGetUniformLocation(vignette_prog, "u_intensity");

    {
        f32 fullscreen_quad[12] = {
            -1.0f, -1.0f,  1.0f, -1.0f,  1.0f, 1.0f,
            -1.0f, -1.0f,  1.0f,  1.0f, -1.0f, 1.0f,
        };
        glGenBuffers(1, &vignette_vbo);
        glBindBuffer(GL_ARRAY_BUFFER, vignette_vbo);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof(fullscreen_quad), fullscreen_quad, GL_STATIC_DRAW);
    }

    level_geo = build_level_geometry();
    level_vertex_count = level_geo.count;
    glGenBuffers(1, &level_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, level_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof(GeoVertex) * (size_t)level_geo.count),
                 level_geo.verts, GL_STATIC_DRAW);
    vertex_list_free(&level_geo);

    glGenBuffers(1, &entity_vbo);
    glGenBuffers(1, &hud_vbo);

    glEnable(GL_DEPTH_TEST);
    {
        int menu_selection  = MENU_OPTION_SOLO;
        int won_selection   = WON_OPTION_QUIT_MENU;
        int net_thread_started = 0;

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
                        g_key_e = g_key_space = g_key_enter = 0;
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
                continue;  
            }

            if (g_app_state == APP_STATE_WON) {
                if (g_menu_nav_up_requested) {
                    g_menu_nav_up_requested = 0;
                    won_selection = (won_selection + WON_OPTION_COUNT - 1) % WON_OPTION_COUNT;
                }
                if (g_menu_nav_down_requested) {
                    g_menu_nav_down_requested = 0;
                    won_selection = (won_selection + 1) % WON_OPTION_COUNT;
                }
                if (g_menu_confirm_requested) {
                    g_menu_confirm_requested = 0;
                    if (won_selection == WON_OPTION_QUIT_GAME) {
                        teardown_and_return_to_menu(net_tid, &net_thread_started);
                        g_running = 0;
                    } else {
                        teardown_and_return_to_menu(net_tid, &net_thread_started);
                    }
                    won_selection = WON_OPTION_QUIT_MENU;   
                    continue;
                }

                if (g_running) {
                    TextVertexList won_text;
                    VertexList won_geo = build_won_geometry(win_w, win_h, won_selection, &won_text);
                    Mat4 identity = mat4_identity();

                    glViewport(0, 0, win_w, win_h);
                    glClearColor(0.04f, 0.06f, 0.05f, 1.0f);   
                    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

                    glUseProgram(prog);
                    glDisable(GL_DEPTH_TEST);
                    glUniformMatrix4fv(mvp_loc, 1, GL_FALSE, identity.m);
                    if (won_geo.count > 0) {
                        glBindBuffer(GL_ARRAY_BUFFER, hud_vbo);
                        glBufferData(GL_ARRAY_BUFFER,
                                    (GLsizeiptr)(sizeof(GeoVertex) * (size_t)won_geo.count),
                                    won_geo.verts, GL_DYNAMIC_DRAW);
                        draw_vertex_list(hud_vbo, pos_loc, color_loc, won_geo.count);
                    }
                    vertex_list_free(&won_geo);

                    glUseProgram(text_prog);
                    draw_text(text_vbo, font_texture, text_pos_loc, text_texcoord_loc,
                             text_color_loc, text_tex_loc, &won_text);
                    text_vlist_free(&won_text);

                    glEnable(GL_DEPTH_TEST);

                    eglSwapBuffers(egl_disp, egl_surf);
                }
                continue;
            }

            if (g_return_to_menu_requested) {
                g_return_to_menu_requested = 0;
                teardown_and_return_to_menu(net_tid, &net_thread_started);
                continue;  
            }

            {
                u8  wave_snapshot;
                int zombie_count_snapshot, has_state_snapshot;
                pthread_mutex_lock(&g_state.lock);
                wave_snapshot           = g_state.wave;
                zombie_count_snapshot   = g_state.zombie_count;
                has_state_snapshot      = g_state.has_my_state;
                pthread_mutex_unlock(&g_state.lock);

                if (has_state_snapshot && wave_snapshot >= WAVE_COUNT && zombie_count_snapshot == 0) {
                    g_app_state = APP_STATE_WON;
                    printf("[main] Wave %d cleared -- victory.\n", WAVE_COUNT);
                    continue;
                }
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
                TextVertexList hud_text;
                VertexList hud_geo    = build_hud_geometry(win_w, win_h, dt, &hud_text);

                glViewport(0, 0, win_w, win_h);
                glClearColor(0.02f, 0.02f, 0.03f, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

                glUseProgram(level_prog);
                glUniformMatrix4fv(level_mvp_loc, 1, GL_FALSE, mvp.m);
                draw_vertex_list_lit(level_vbo, level_pos_loc, level_normal_loc, level_color_loc,
                                     level_vertex_count);

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

                glUseProgram(text_prog);
                draw_text(text_vbo, font_texture, text_pos_loc, text_texcoord_loc,
                         text_color_loc, text_tex_loc, &hud_text);
                text_vlist_free(&hud_text);

                g_damage_flash -= dt / DAMAGE_FLASH_DECAY_TIME;
                if (g_damage_flash < 0.0f) g_damage_flash = 0.0f;
                if (g_damage_flash > 0.0f) {
                    glUseProgram(vignette_prog);
                    glUniform1f(vignette_intensity_loc, g_damage_flash);
                    glEnable(GL_BLEND);
                    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                    glBindBuffer(GL_ARRAY_BUFFER, vignette_vbo);
                    glVertexAttribPointer((GLuint)vignette_pos_loc, 2, GL_FLOAT, GL_FALSE, 0, (const void *)0);
                    glEnableVertexAttribArray((GLuint)vignette_pos_loc);
                    glDrawArrays(GL_TRIANGLES, 0, 6);
                    glDisable(GL_BLEND);   
                }

                glEnable(GL_DEPTH_TEST);   
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