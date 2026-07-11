/*
 * qnx_client/src/main.c -- STAGE 2a: real level rendering + a local
 * WASD/mouse-look camera. Builds on Stage 1 (validated working: GLES2
 * rendering, Screen's own input event queue, UDP handshake) without
 * touching any of that -- the network thread below is UNCHANGED from
 * Stage 1 and still just sends a heartbeat input, on purpose. Wiring
 * the camera's real movement into PktInput, and rendering other
 * players/zombies from server state, is Stage 2b, once this stage is
 * confirmed working on hardware the same way Stage 1 was.
 *
 * What's new this stage:
 *   - A GLES2 shader pipeline (position + flat vertex color, one MVP
 *     uniform) -- GLES2 has no fixed-function matrix stack, so this
 *     is hand-rolled via mat4.h.
 *   - The actual level geometry from src/map.h/map.c (walls, floor,
 *     ceiling, ramps, platforms), built once at startup as a static
 *     vertex buffer -- see level_geo.c.
 *   - A free camera: WASD moves relative to yaw, mouse-look rotates
 *     yaw/pitch, and standing on a ramp/platform tile smoothly raises
 *     the camera's height (same tile-based approximation
 *     src/server.c's zombie_height_tick() uses for zombies).
 *   - Basic wall collision via map_is_wall(), same per-axis-slide
 *     approach src/server.c's player_move() uses.
 *
 * KNOWN LIMITATION, called out honestly rather than silently: mouse
 * look is computed by diffing consecutive SCREEN_PROPERTY_POSITION
 * reads (absolute position), not a raw relative-delta property. I
 * could not find clear QNX documentation for a pointer-recentering/
 * warp API to solve the "mouse gets stuck at the screen edge" problem
 * a real FPS needs, so that limitation is still here -- worth fixing
 * once you can test whether it's actually as annoying in practice as
 * it sounds.
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

#define WINDOW_W 1280   /* fallback only -- actual size is auto-detected from
                         * the display at startup, see main()'s Screen setup */
#define WINDOW_H 720

#define EYE_HEIGHT   1.4f    /* matches src/server.c's PLAYER_EYE_HEIGHT */
#define MOVE_SPEED   3.0f    /* world units/sec -- local free-cam only in
                              * this stage, not yet tied to server MOVE_SPEED */
#define CLIMB_SPEED  1.0f    /* matches src/server.c's ZOMBIE_CLIMB_SPEED */
#define MOUSE_SENS   0.003f
#define LOOK_SPEED   2.0f    /* radians/sec, arrow-key look -- fallback/
                              * supplement for mouse-look, since mouse
                              * position gets stuck at the screen edge
                              * (see the README for why: no documented
                              * QNX Screen API pointer-warp/recenter call
                              * was found for this) */

/* ------------------------------------------------------------------ */
/* Networking thread -- UNCHANGED from Stage 1. Still just a heartbeat */
/* input to keep the connection alive and prove state flows both      */
/* ways; real camera movement gets wired into this in Stage 2b.       */
/* ------------------------------------------------------------------ */
static volatile int g_running   = 1;
static volatile int g_connected = 0;
static volatile u8  g_my_id     = 0xFF;
static volatile f32 g_spawn_x, g_spawn_y, g_spawn_angle;

static int                g_sock = -1;
static struct sockaddr_in g_server_addr;

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

        memset(&inp, 0, sizeof(inp));
        inp.hdr.type      = PKT_INPUT;
        inp.hdr.player_id = g_my_id;
        inp.hdr.tick      = local_tick++;
        inp.look_angle    = g_spawn_angle;
        sendto(g_sock, &inp, sizeof(inp), 0,
               (struct sockaddr *)&g_server_addr, sizeof(g_server_addr));

        while ((n = recvfrom(g_sock, buf, sizeof(buf), 0, NULL, NULL)) > 0) {
            if (n < (ssize_t)sizeof(PktHeader)) continue;
            PktHeader *hdr = (PktHeader *)buf;
            if (hdr->type == PKT_STATE && n >= (ssize_t)(sizeof(PktHeader) + 1)) {
                PktState *st = (PktState *)buf;
                printf("[net] tick=%u players=%u zombies=%u wave=%u\n",
                       hdr->tick, st->player_count, st->zombie_count, st->wave);
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
/* GLES2 shader pipeline -- flat vertex color, one MVP uniform. No     */
/* lighting/texturing, matching the flat-colored-boxes look already    */
/* established on the Godot client.                                    */
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

/* ------------------------------------------------------------------ */
/* Camera + input state                                                */
/* ------------------------------------------------------------------ */
typedef struct {
    f32 x, y, z;
    f32 yaw, pitch;
} Camera;

static int g_key_w = 0, g_key_a = 0, g_key_s = 0, g_key_d = 0;
static int g_key_look_left = 0, g_key_look_right = 0, g_key_look_up = 0, g_key_look_down = 0;
static int g_have_last_pointer = 0;
static int g_last_pointer_x = 0, g_last_pointer_y = 0;

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
    else if (sym == KEYCODE_LEFT)  g_key_look_left  = down;
    else if (sym == KEYCODE_RIGHT) g_key_look_right = down;
    else if (sym == KEYCODE_UP)    g_key_look_up    = down;
    else if (sym == KEYCODE_DOWN)  g_key_look_down  = down;
    else if (sym == KEYCODE_ESCAPE && down) g_running = 0;
}

static void handle_pointer_event(screen_event_t ev, Camera *cam)
{
    int pos[2] = { 0, 0 };
    screen_get_event_property_iv(ev, SCREEN_PROPERTY_POSITION, pos);

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

/* Moves the camera per the currently-held WASD keys, with wall
 * collision via map_is_wall() (same per-axis-slide approach
 * src/server.c's player_move() uses), then eases the camera's height
 * toward whatever the tile underfoot implies (see
 * level_height_for_tile()). */
static void update_camera(Camera *cam, f32 dt)
{
    f32 fx, fz, rx, rz;
    f32 mvx = 0.0f, mvz = 0.0f;
    f32 len;
    f32 new_x, new_z, target_y, climb_step;
    int tile_mx, tile_my;

    /* Arrow-key look -- unaffected by the mouse's edge-of-screen
     * limitation since it's driven by held-key state, not cursor
     * position. Applied before movement so this frame's WASD uses
     * the just-updated yaw. */
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
    climb_step = CLIMB_SPEED * dt;
    if (cam->y < target_y) {
        cam->y += climb_step;
        if (cam->y > target_y) cam->y = target_y;
    } else if (cam->y > target_y) {
        cam->y -= climb_step;
        if (cam->y < target_y) cam->y = target_y;
    }
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
    GLuint  level_vbo;
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

    /* Query the real display resolution rather than trusting the
     * WINDOW_W/WINDOW_H fallback -- SCREEN_PROPERTY_SIZE on a DISPLAY
     * object (not a window) reports "the width and height, in pixels,
     * of the current video resolution" per QNX's own Screen API docs.
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

    /* ---- EGL setup (unchanged from Stage 1) ---- */
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

    /* ---- shader + level geometry ---- */
    prog = build_shader_program(&mvp_loc, &pos_loc, &color_loc);
    if (!prog) { fprintf(stderr, "Shader setup failed\n"); return 1; }

    level_geo = build_level_geometry();
    glGenBuffers(1, &level_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, level_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof(GeoVertex) * (size_t)level_geo.count),
                 level_geo.verts, GL_STATIC_DRAW);
    vertex_list_free(&level_geo);   /* uploaded to the GPU; don't need the CPU copy anymore */

    glEnable(GL_DEPTH_TEST);
    /* Deliberately NOT calling glEnable(GL_CULL_FACE) -- see the
     * comment on FACES in level_geo.c for why. */

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

    last_time = portable_time();

    printf("[main] Entering render loop. WASD to move, mouse to look, ESC to quit.\n");
    while (g_running) {
        double now = portable_time();
        f32 dt = (f32)(now - last_time);
        last_time = now;
        if (dt > 0.1f) dt = 0.1f;   /* clamp in case of a hitch/breakpoint pause */

        for (;;) {
            int type = SCREEN_EVENT_NONE;
            if (screen_get_event(screen_ctx, screen_ev, 0) != 0) break;
            screen_get_event_property_iv(screen_ev, SCREEN_PROPERTY_TYPE, &type);
            if (type == SCREEN_EVENT_NONE) break;

            if (type == SCREEN_EVENT_KEYBOARD) handle_keyboard_event(screen_ev, &cam);
            else if (type == SCREEN_EVENT_POINTER) handle_pointer_event(screen_ev, &cam);
        }

        update_camera(&cam, dt);

        {
            Mat4 proj = mat4_perspective(70.0f * 3.14159265f / 180.0f,
                                        (f32)win_w / (f32)win_h, 0.1f, 100.0f);
            Mat4 view = mat4_view_from_yaw_pitch(cam.x, cam.y, cam.z, cam.yaw, cam.pitch);
            Mat4 mvp  = mat4_multiply(proj, view);

            glViewport(0, 0, win_w, win_h);
            glClearColor(0.02f, 0.02f, 0.03f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

            glUseProgram(prog);
            glUniformMatrix4fv(mvp_loc, 1, GL_FALSE, mvp.m);

            glBindBuffer(GL_ARRAY_BUFFER, level_vbo);
            glVertexAttribPointer((GLuint)pos_loc, 3, GL_FLOAT, GL_FALSE,
                                  sizeof(GeoVertex), (const void *)0);
            glEnableVertexAttribArray((GLuint)pos_loc);
            glVertexAttribPointer((GLuint)color_loc, 3, GL_FLOAT, GL_FALSE,
                                  sizeof(GeoVertex), (const void *)(3 * sizeof(f32)));
            glEnableVertexAttribArray((GLuint)color_loc);

            /* level_vbo's vertex count was freed along with the CPU
             * copy above -- recompute the draw count from the buffer
             * size instead of keeping a separate variable around. */
            {
                GLint buf_size = 0;
                glGetBufferParameteriv(GL_ARRAY_BUFFER, GL_BUFFER_SIZE, &buf_size);
                glDrawArrays(GL_TRIANGLES, 0, buf_size / (GLsizei)sizeof(GeoVertex));
            }
        }

        eglSwapBuffers(egl_disp, egl_surf);
    }

    g_running = 0;
    pthread_join(net_tid, NULL);

    eglMakeCurrent(egl_disp, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(egl_disp, egl_surf);
    eglDestroyContext(egl_disp, egl_ctx);
    eglTerminate(egl_disp);
    screen_destroy_window(screen_win);
    screen_destroy_context(screen_ctx);
    close(g_sock);
    return 0;
}
