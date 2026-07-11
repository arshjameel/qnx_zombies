/*
 * headless coop game server
 * determines all game logic
 */

#include "common.h"
#include "net.h"
#include "map.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "qnx_time.h"
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* ------------------------------------------------------------------ */
/* Game constants                                                       */
/* ------------------------------------------------------------------ */
#define MOVE_SPEED      0.05f
#define SHOOT_DAMAGE    25
#define SHOOT_RANGE     10.0f
#define AMMO_MAX        30
#define RESPAWN_TICKS   (NET_TICK_RATE * 3)   // 3 seconds

#define ZOMBIE_SPEED              0.025f
#define ZOMBIE_HEALTH             50
#define ZOMBIE_DAMAGE             10
#define ZOMBIE_ATTACK_RANGE       0.8f
#define ZOMBIE_ATTACK_COOLDOWN    (NET_TICK_RATE * 1)  // 1 attack/sec
#define WAVE_BASE_ZOMBIES         3
#define WAVE_ZOMBIE_INCREMENT     2

#define SPAWN_COUNT 4
static const f32 SPAWN_X[SPAWN_COUNT] = { 2.5f, 21.5f,  2.5f, 21.5f };
static const f32 SPAWN_Y[SPAWN_COUNT] = { 2.5f,  2.5f, 21.5f, 21.5f };
static const f32 SPAWN_A[SPAWN_COUNT] = { 0.0f,  3.14f,  1.57f, 4.71f };

/* zombie spawn points spread across the middle of the map and away from player spawn corners. */
#define ZOMBIE_SPAWN_COUNT 8
static const f32 ZOMBIE_SPAWN_X[ZOMBIE_SPAWN_COUNT] =
    { 8.5f, 15.5f, 4.5f, 12.5f, 19.5f, 8.5f, 15.5f, 12.5f };
static const f32 ZOMBIE_SPAWN_Y[ZOMBIE_SPAWN_COUNT] =
    { 2.5f,  2.5f, 11.5f, 11.5f, 11.5f, 21.5f, 21.5f, 20.5f };

/* ------------------------------------------------------------------ */
/* Server player                                                        */
/* ------------------------------------------------------------------ */
typedef struct {
    int                active;
    struct sockaddr_in addr;
    f32                x, y, angle;
    u8                 health;
    u8                 ammo;
    int                alive;
    int                respawn_timer;
    /* buffered input from latest PKT_INPUT */
    u8                 inp_forward;
    u8                 inp_back;
    u8                 inp_strafe_l;
    u8                 inp_strafe_r;
    f32                inp_look_angle;
    u8                 inp_shoot;
    u8                 inp_shoot_prev;
} ServerPlayer;

typedef struct {
    int active;
    int alive;
    f32 x, y;
    u8  health;
    int attack_cooldown;
} ServerZombie;

static ServerPlayer g_players[NET_MAX_PLAYERS];
static ServerZombie g_zombies[MAX_ZOMBIES];
static u8           g_wave = 0;
static int          g_sock = -1;
static u32          g_tick = 0;

/* ------------------------------------------------------------------ */
/* Helper functions                                                     */
/* ------------------------------------------------------------------ */
static double mono_time(void)
{
    return portable_time();
}

static int find_player(struct sockaddr_in *addr)
{
    int i;
    for (i = 0; i < NET_MAX_PLAYERS; i++) {
        if (!g_players[i].active) continue;
        if (g_players[i].addr.sin_addr.s_addr == addr->sin_addr.s_addr &&
            g_players[i].addr.sin_port        == addr->sin_port)
            return i;
    }
    return -1;
}

static int alloc_player(struct sockaddr_in *addr)
{
    int i;
    for (i = 0; i < NET_MAX_PLAYERS; i++) {
        if (g_players[i].active) continue;
        memset(&g_players[i], 0, sizeof(g_players[i]));
        g_players[i].active = 1;
        g_players[i].addr   = *addr;
        g_players[i].x      = SPAWN_X[i % SPAWN_COUNT];
        g_players[i].y      = SPAWN_Y[i % SPAWN_COUNT];
        g_players[i].angle  = SPAWN_A[i % SPAWN_COUNT];
        g_players[i].inp_look_angle = g_players[i].angle;
        g_players[i].health = 100;
        g_players[i].ammo   = AMMO_MAX;
        g_players[i].alive  = 1;
        return i;
    }
    return -1;   /* server full */
}

/* slide against walls independently on each axis */
static void player_move(ServerPlayer *p, f32 dx, f32 dy)
{
    if (!map_is_wall((int)(p->x + dx), (int)p->y)) p->x += dx;
    if (!map_is_wall((int)p->x, (int)(p->y + dy))) p->y += dy;
}

static int count_alive_zombies(void)
{
    int i, n = 0;
    for (i = 0; i < MAX_ZOMBIES; i++)
        if (g_zombies[i].active && g_zombies[i].alive) n++;
    return n;
}

static int any_player_connected(void)
{
    int i;
    for (i = 0; i < NET_MAX_PLAYERS; i++)
        if (g_players[i].active) return 1;
    return 0;
}

static void spawn_wave(void)
{
    int want    = WAVE_BASE_ZOMBIES + (int)g_wave * WAVE_ZOMBIE_INCREMENT;
    int spawned = 0, i;

    for (i = 0; i < MAX_ZOMBIES && spawned < want; i++) {
        if (g_zombies[i].active) continue;
        g_zombies[i].active          = 1;
        g_zombies[i].alive           = 1;
        g_zombies[i].health          = ZOMBIE_HEALTH;
        g_zombies[i].x               = ZOMBIE_SPAWN_X[i % ZOMBIE_SPAWN_COUNT];
        g_zombies[i].y               = ZOMBIE_SPAWN_Y[i % ZOMBIE_SPAWN_COUNT];
        g_zombies[i].attack_cooldown = 0;
        spawned++;
    }
    g_wave++;
    printf("Wave %d started: %d zombies\n", g_wave, spawned);
}

/* Hitscan 
 * return id of nearest alive zombie in shooter's forward cone 
 * or -1 if no zombie in that cone 
 */
static int shoot_check(int shooter_id)
{
    ServerPlayer *sh = &g_players[shooter_id];
    f32 rx = cosf(sh->angle);
    f32 ry = sinf(sh->angle);
    f32 best = SHOOT_RANGE;
    int hit  = -1;
    int i;

    for (i = 0; i < MAX_ZOMBIES; i++) {
        if (!g_zombies[i].active || !g_zombies[i].alive) continue;
        f32 dx   = g_zombies[i].x - sh->x;
        f32 dy   = g_zombies[i].y - sh->y;
        f32 dist = sqrtf(dx*dx + dy*dy);
        if (dist >= best) continue;
        f32 dot  = (dx/dist)*rx + (dy/dist)*ry;
        if (dot < 0.9f) continue;   // ~26 degree half angle cone
        best = dist;
        hit  = i;
    }
    return hit;
}

/* ------------------------------------------------------------------ */
/* Packet sending                                                       */
/* ------------------------------------------------------------------ */
static void send_to(int player_id, const void *pkt, size_t len)
{
    sendto(g_sock, pkt, len, 0,
           (struct sockaddr *)&g_players[player_id].addr,
           sizeof(g_players[player_id].addr));
}

static void broadcast(const void *pkt, size_t len)
{
    int i;
    for (i = 0; i < NET_MAX_PLAYERS; i++)
        if (g_players[i].active)
            send_to(i, pkt, len);
}

static void send_accept(int id)
{
    PktAccept pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.hdr.type      = PKT_ACCEPT;
    pkt.hdr.player_id = 0;
    pkt.hdr.tick      = g_tick;
    pkt.assigned_id   = (u8)id;
    pkt.spawn_x       = g_players[id].x;
    pkt.spawn_y       = g_players[id].y;
    pkt.spawn_angle   = g_players[id].angle;
    send_to(id, &pkt, sizeof(pkt));
}

static void broadcast_state(void)
{
    PktState pkt;
    int      active = 0, zactive = 0, i;
    memset(&pkt, 0, sizeof(pkt));
    pkt.hdr.type = PKT_STATE;
    pkt.hdr.tick = g_tick;

    for (i = 0; i < NET_MAX_PLAYERS; i++) {
        if (!g_players[i].active) continue;
        PlayerState *ps = &pkt.players[active++];
        ps->player_id = (u8)i;
        ps->alive     = (u8)g_players[i].alive;
        ps->x         = g_players[i].x;
        ps->y         = g_players[i].y;
        ps->angle     = g_players[i].angle;
        ps->health    = g_players[i].health;
        ps->ammo      = g_players[i].ammo;
    }
    pkt.player_count = (u8)active;

    for (i = 0; i < MAX_ZOMBIES; i++) {
        if (!g_zombies[i].active) continue;
        ZombieState *zs = &pkt.zombies[zactive++];
        zs->zombie_id = (u8)i;
        zs->alive     = (u8)g_zombies[i].alive;
        zs->x         = g_zombies[i].x;
        zs->y         = g_zombies[i].y;
        zs->health    = g_zombies[i].health;
    }
    pkt.zombie_count = (u8)zactive;
    pkt.wave         = g_wave;

    broadcast(&pkt, sizeof(pkt));
}

static void broadcast_hit(u8 victim_id, u8 victim_type, u8 attacker_id, u8 damage)
{
    PktHit pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.hdr.type     = PKT_HIT;
    pkt.hdr.tick     = g_tick;
    pkt.victim_id    = victim_id;
    pkt.victim_type  = victim_type;
    pkt.attacker_id  = attacker_id;
    pkt.damage       = damage;
    broadcast(&pkt, sizeof(pkt));
}

/* ------------------------------------------------------------------ */
/* Receive loop                                                         */
/* ------------------------------------------------------------------ */
static void recv_packets(void)
{
    u8                 buf[1024];
    struct sockaddr_in from;
    socklen_t          from_len = sizeof(from);
    ssize_t            n;

    while ((n = recvfrom(g_sock, buf, sizeof(buf), 0,
                         (struct sockaddr *)&from, &from_len)) > 0) {
        if (n < (ssize_t)sizeof(PktHeader)) continue;
        PktHeader *hdr = (PktHeader *)buf;

        switch (hdr->type) {

        case PKT_CONNECT: {
            int id = find_player(&from);
            if (id < 0) {
                int first = !any_player_connected();
                id = alloc_player(&from);
                if (id < 0) { fprintf(stderr, "Server full\n"); break; }
                printf("Player %d connected  %s:%d\n",
                       id, inet_ntoa(from.sin_addr), ntohs(from.sin_port));
                if (first && count_alive_zombies() == 0)
                    spawn_wave();
            }
            send_accept(id);
            break;
        }

        case PKT_INPUT: {
            if (n < (ssize_t)sizeof(PktInput)) break;
            int id = find_player(&from);
            if (id < 0) break;
            PktInput *inp = (PktInput *)buf;
            g_players[id].inp_forward    = inp->forward;
            g_players[id].inp_back       = inp->back;
            g_players[id].inp_strafe_l   = inp->strafe_left;
            g_players[id].inp_strafe_r   = inp->strafe_right;
            g_players[id].inp_look_angle = inp->look_angle;
            g_players[id].inp_shoot      = inp->shoot;
            break;
        }

        case PKT_DISCONNECT: {
            int id = find_player(&from);
            if (id >= 0) {
                printf("Player %d disconnected\n", id);
                g_players[id].active = 0;
            }
            break;
        }

        default:
            DLOG("Unknown packet type 0x%02X", hdr->type);
            break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Zombie AI tick                                                       */
/* ------------------------------------------------------------------ */
static void zombie_tick(void)
{
    int i, p;
    for (i = 0; i < MAX_ZOMBIES; i++) {
        ServerZombie *z = &g_zombies[i];
        if (!z->active || !z->alive) continue;

        /* find nearest alive player */
        int target = -1;
        f32 best_dist = 1e9f;
        for (p = 0; p < NET_MAX_PLAYERS; p++) {
            if (!g_players[p].active || !g_players[p].alive) continue;
            f32 dx = g_players[p].x - z->x;
            f32 dy = g_players[p].y - z->y;
            f32 d  = sqrtf(dx*dx + dy*dy);
            if (d < best_dist) { best_dist = d; target = p; }
        }
        if (target < 0) continue;   /* nobody alive to chase */

        f32 dx   = g_players[target].x - z->x;
        f32 dy   = g_players[target].y - z->y;
        f32 dist = best_dist;

        if (dist > ZOMBIE_ATTACK_RANGE) {
            f32 nx = z->x + (dx / dist) * ZOMBIE_SPEED;
            f32 ny = z->y + (dy / dist) * ZOMBIE_SPEED;
            if (!map_is_wall((int)nx, (int)z->y)) z->x = nx;
            if (!map_is_wall((int)z->x, (int)ny)) z->y = ny;
        } else if (z->attack_cooldown <= 0) {
            ServerPlayer *pl = &g_players[target];
            u8 dmg = ZOMBIE_DAMAGE;
            pl->health = (pl->health > dmg) ? pl->health - dmg : 0;
            broadcast_hit((u8)target, ENTITY_PLAYER, ATTACKER_ZOMBIE, dmg);
            if (pl->health == 0) {
                pl->alive         = 0;
                pl->respawn_timer = RESPAWN_TICKS;
                printf("Player %d was overrun by zombies\n", target);
            }
            z->attack_cooldown = ZOMBIE_ATTACK_COOLDOWN;
        }
        if (z->attack_cooldown > 0) z->attack_cooldown--;
    }

    /* Next wave once the current one is fully cleared (and someone's playing) */
    if (any_player_connected() && count_alive_zombies() == 0)
        spawn_wave();
}

/* ------------------------------------------------------------------ */
/* Game tick (called NET_TICK_RATE times per second)                   */
/* ------------------------------------------------------------------ */
static void game_tick(void)
{
    int i;
    for (i = 0; i < NET_MAX_PLAYERS; i++) {
        ServerPlayer *p = &g_players[i];
        if (!p->active) continue;

        if (!p->alive) {
            if (--p->respawn_timer <= 0) {
                p->x      = SPAWN_X[i % SPAWN_COUNT];
                p->y      = SPAWN_Y[i % SPAWN_COUNT];
                p->angle  = SPAWN_A[i % SPAWN_COUNT];
                p->health = 100;
                p->ammo   = AMMO_MAX;
                p->alive  = 1;
                DLOG("Player %d respawned", i);
            }
            continue;
        }

        p->angle = p->inp_look_angle;

        if (p->inp_forward)  player_move(p,  cosf(p->angle)*MOVE_SPEED,  sinf(p->angle)*MOVE_SPEED);
        if (p->inp_back)     player_move(p, -cosf(p->angle)*MOVE_SPEED, -sinf(p->angle)*MOVE_SPEED);
        if (p->inp_strafe_l) player_move(p,  sinf(p->angle)*MOVE_SPEED, -cosf(p->angle)*MOVE_SPEED);
        if (p->inp_strafe_r) player_move(p, -sinf(p->angle)*MOVE_SPEED,  cosf(p->angle)*MOVE_SPEED);

        /* Rising-edge shoot */
        if (p->inp_shoot && !p->inp_shoot_prev && p->ammo > 0) {
            p->ammo--;
            int hit = shoot_check(i);
            if (hit >= 0) {
                u8 dmg = SHOOT_DAMAGE;
                ServerZombie *z = &g_zombies[hit];
                z->health = (z->health > dmg) ? z->health - dmg : 0;
                broadcast_hit((u8)hit, ENTITY_ZOMBIE, (u8)i, dmg);
                if (z->health == 0) {
                    z->alive  = 0;
                    z->active = 0;
                    printf("Zombie %d killed by player %d\n", hit, i);
                }
            }
        }
        p->inp_shoot_prev = p->inp_shoot;
    }

    zombie_tick();
}

/* ------------------------------------------------------------------ */
/* main                                                                 */
/* ------------------------------------------------------------------ */
int main(int argc, char **argv)
{
    u16 port = NET_PORT;
    if (argc > 1) port = (u16)atoi(argv[1]);

    memset(g_players, 0, sizeof(g_players));
    memset(g_zombies, 0, sizeof(g_zombies));

    g_sock = net_udp_socket();
    if (g_sock < 0) return 1;
    if (net_bind(g_sock, port) < 0) return 1;

    printf("qnx-game server  port=%d  tick_rate=%d Hz\n",
           port, NET_TICK_RATE);

    double tick_interval = 1.0 / NET_TICK_RATE;
    double next_tick     = mono_time() + tick_interval;

    for (;;) {
        recv_packets();

        double now = mono_time();
        if (now >= next_tick) {
            game_tick();
            broadcast_state();
            g_tick++;
            next_tick += tick_interval;
        } else {
            portable_sleep_ms(1);
        }
    }

    return 0;
}
