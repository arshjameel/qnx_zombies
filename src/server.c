/*
 * server.c -- authoritative headless coop game server
 *
 * Runs on QNX Pi5 (or any POSIX host). No display, no platform layer.
 * All connected clients (Godot, on any OS) share ONE world. "Solo" vs
 * "Coop" is purely a client-side menu concept -- this server does not
 * distinguish sessions, it just simulates whoever is connected.
 *
 * Usage: ./server [port]
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
/*                                                                      */
/* COUPLING WARNING: MOVE_SPEED only affects the server's authoritative */
/* position. The Godot client predicts its own movement locally for    */
/* responsiveness (see godot_client/scripts/Player.gd's PLAYER_SPEED)  */
/* rather than waiting on the network each frame, so changing this     */
/* value alone won't visibly change movement -- you'll just see subtle */
/* rubber-banding as the server's truth drifts from the client's guess.*/
/* Keep them in sync: PLAYER_SPEED (world units/sec) = MOVE_SPEED       */
/* (map units/tick) * NET_TICK_RATE (ticks/sec).                       */
/* ------------------------------------------------------------------ */
#define MOVE_SPEED      0.25f
#define SHOOT_DAMAGE    25
#define SHOOT_RANGE     100.0f /* can shoot long distances */
#define AMMO_MAX        60
#define RESPAWN_TICKS   (NET_TICK_RATE * 3)   /* 3 seconds */

/* ------------------------------------------------------------------ */
/* Pickups -- ammo/health packs. Spawn locations come from the map's   */
/* tile-5 markers (see map_get_pickup_spawns), one pickup slot per     */
/* marker, replicated per session so coop and every solo world each    */
/* get their own independent set that doesn't interfere with others.   */
/* ------------------------------------------------------------------ */
#define PICKUP_RADIUS         0.6f
#define PICKUP_RESPAWN_TICKS  (NET_TICK_RATE * 12)  /* 12 seconds */
#define AMMO_PICKUP_AMOUNT    15
#define HEALTH_PICKUP_AMOUNT  25

#define ZOMBIE_SPEED              0.05f
#define ZOMBIE_HEALTH             100
#define ZOMBIE_DAMAGE             20
#define ZOMBIE_ATTACK_RANGE       0.8f
#define ZOMBIE_ATTACK_COOLDOWN    (NET_TICK_RATE * 1)  /* 1 attack/sec */
#define WAVE_BASE_ZOMBIES         3
#define WAVE_ZOMBIE_INCREMENT     2

/* Head/body hitbox split. The server has no real 3D geometry -- height
 * only ever existed as a client-side rendering convention -- so this
 * is a flat approximation: given the shooter's eye height, vertical
 * aim (pitch), and horizontal distance to the target (already known
 * from shoot_check's cone test), we compute where a straight line at
 * that pitch would cross the target's vertical column, then classify
 * that height against the zombie's head/body split.
 *
 * COUPLING WARNING: PLAYER_EYE_HEIGHT must match Player.gd's camera
 * height (PLAYER_HEIGHT - 0.2), and ZOMBIE_HEIGHT must match
 * Zombie.gd's HEIGHT constant. If those drift apart, headshots will
 * be classified against a target that's taller/shorter on screen than
 * what the server thinks it's aiming at. */
#define PLAYER_EYE_HEIGHT   1.4f
#define ZOMBIE_HEIGHT        1.8f
#define HEAD_ZONE_FRACTION   0.25f   /* top quarter of ZOMBIE_HEIGHT is head */
#define HEADSHOT_DAMAGE       50

#define SPAWN_COUNT 4
static const f32 SPAWN_X[SPAWN_COUNT] = { 2.5f, 21.5f,  2.5f, 21.5f };
static const f32 SPAWN_Y[SPAWN_COUNT] = { 2.5f,  2.5f, 21.5f, 21.5f };
static const f32 SPAWN_A[SPAWN_COUNT] = { 0.0f,  3.14f,  1.57f, 4.71f };

/* Zombie spawn points -- all verified open floor tiles in map.c,
 * spread across the middle of the map, away from player spawn corners. */
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
    u8                 session_id;   /* 0 = coop (shared); N+1 = solo player N's private world */
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
    f32                inp_pitch;
    u8                 inp_shoot;
    u8                 inp_shoot_prev;
} ServerPlayer;

typedef struct {
    int active;
    int alive;
    u8  session_id;
    f32 x, y;
    u8  health;
    int attack_cooldown;
} ServerZombie;

/* One pickup slot per map marker. type/x/y are fixed at startup (from
 * the map); active/respawn_timer are the only things that change per
 * session, so this is indexed [session_id][spawn_index] rather than
 * being a dynamic free-list like g_zombies -- there's always exactly
 * one pickup "slot" at each marker, it's just on cooldown or not. */
typedef struct {
    int active;
    int respawn_timer;
} ServerPickup;

static ServerPlayer g_players[NET_MAX_PLAYERS];
static ServerZombie g_zombies[MAX_ZOMBIES];

static f32 g_pickup_spawn_x[MAX_PICKUP_SPAWNS];
static f32 g_pickup_spawn_y[MAX_PICKUP_SPAWNS];
static u8  g_pickup_spawn_type[MAX_PICKUP_SPAWNS];
static int g_pickup_spawn_count = 0;

/* Session 0 = coop (shared world). Sessions 1..NET_MAX_PLAYERS are
 * private solo worlds, one per player slot -- a solo player's session
 * id is just their own slot index + 1, which is trivially unique
 * without needing room codes or a lobby system. */
#define SESSION_COOP  0u
#define SESSION_COUNT (NET_MAX_PLAYERS + 1)

/* [session_id][spawn_index] -- see ServerPickup comment above. */
static ServerPickup g_pickups[SESSION_COUNT][MAX_PICKUP_SPAWNS];

static u8           g_wave[SESSION_COUNT];
static int          g_sock = -1;
static u32          g_tick = 0;

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
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

static int alloc_player(struct sockaddr_in *addr, u8 mode)
{
    int i;
    for (i = 0; i < NET_MAX_PLAYERS; i++) {
        if (g_players[i].active) continue;
        memset(&g_players[i], 0, sizeof(g_players[i]));
        g_players[i].active     = 1;
        g_players[i].addr       = *addr;
        g_players[i].session_id = (mode == CONNECT_MODE_SOLO) ? (u8)(i + 1) : SESSION_COOP;
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

/* Attempt move; slide against walls independently on each axis */
static void player_move(ServerPlayer *p, f32 dx, f32 dy)
{
    if (!map_is_wall((int)(p->x + dx), (int)p->y)) p->x += dx;
    if (!map_is_wall((int)p->x, (int)(p->y + dy))) p->y += dy;
}

static int count_alive_zombies_in(u8 session_id)
{
    int i, n = 0;
    for (i = 0; i < MAX_ZOMBIES; i++)
        if (g_zombies[i].active && g_zombies[i].alive &&
            g_zombies[i].session_id == session_id) n++;
    return n;
}

static int any_player_connected_in(u8 session_id)
{
    int i;
    for (i = 0; i < NET_MAX_PLAYERS; i++)
        if (g_players[i].active && g_players[i].session_id == session_id) return 1;
    return 0;
}

static void spawn_wave(u8 session_id)
{
    int want    = WAVE_BASE_ZOMBIES + (int)g_wave[session_id] * WAVE_ZOMBIE_INCREMENT;
    int spawned = 0, i;

    for (i = 0; i < MAX_ZOMBIES && spawned < want; i++) {
        if (g_zombies[i].active) continue;
        g_zombies[i].active          = 1;
        g_zombies[i].alive           = 1;
        g_zombies[i].session_id      = session_id;
        g_zombies[i].health          = ZOMBIE_HEALTH;
        g_zombies[i].x               = ZOMBIE_SPAWN_X[i % ZOMBIE_SPAWN_COUNT];
        g_zombies[i].y               = ZOMBIE_SPAWN_Y[i % ZOMBIE_SPAWN_COUNT];
        g_zombies[i].attack_cooldown = 0;
        spawned++;
    }
    g_wave[session_id]++;
    printf("Session %d: wave %d started, %d zombies\n", session_id, g_wave[session_id], spawned);
}

/* Called once at startup. Reads pickup spawn points from the map and
 * marks every session's copy of each slot as active, alternating ammo
 * and health so packs are a mix rather than all-ammo or all-health. */
static void init_pickups(void)
{
    int i, s;
    g_pickup_spawn_count = map_get_pickup_spawns(g_pickup_spawn_x, g_pickup_spawn_y);
    for (i = 0; i < g_pickup_spawn_count; i++)
        g_pickup_spawn_type[i] = (i % 2 == 0) ? PICKUP_AMMO : PICKUP_HEALTH;

    for (s = 0; s < SESSION_COUNT; s++)
        for (i = 0; i < g_pickup_spawn_count; i++) {
            g_pickups[s][i].active        = 1;
            g_pickups[s][i].respawn_timer = 0;
        }
    printf("Loaded %d pickup spawn points from map\n", g_pickup_spawn_count);
}

/* Checks one player against every pickup in their session and applies
 * whichever packs they're standing on. Only consumes a pack if it
 * actually helps (below max ammo/health) -- so a topped-up player
 * passing through leaves it for a teammate who needs it more. */
static void pickup_tick_for_player(ServerPlayer *p)
{
    int i;
    for (i = 0; i < g_pickup_spawn_count; i++) {
        ServerPickup *pk = &g_pickups[p->session_id][i];
        if (!pk->active) continue;

        f32 dx = g_pickup_spawn_x[i] - p->x;
        f32 dy = g_pickup_spawn_y[i] - p->y;
        if (dx*dx + dy*dy > PICKUP_RADIUS * PICKUP_RADIUS) continue;

        u8 type = g_pickup_spawn_type[i];
        int applied = 0;

        if (type == PICKUP_AMMO && p->ammo < AMMO_MAX) {
            int new_ammo = (int)p->ammo + AMMO_PICKUP_AMOUNT;
            p->ammo = (u8)MIN(new_ammo, AMMO_MAX);
            applied = 1;
        } else if (type == PICKUP_HEALTH && p->health < 100) {
            int new_health = (int)p->health + HEALTH_PICKUP_AMOUNT;
            p->health = (u8)MIN(new_health, 100);
            applied = 1;
        }

        if (applied) {
            pk->active        = 0;
            pk->respawn_timer = PICKUP_RESPAWN_TICKS;
        }
    }
}

/* Ticks down respawn timers for every session's pickups. Cheap to run
 * unconditionally (SESSION_COUNT * MAX_PICKUP_SPAWNS is tiny). */
static void pickup_respawn_tick(void)
{
    int s, i;
    for (s = 0; s < SESSION_COUNT; s++)
        for (i = 0; i < g_pickup_spawn_count; i++) {
            ServerPickup *pk = &g_pickups[s][i];
            if (pk->active) continue;
            if (--pk->respawn_timer <= 0) pk->active = 1;
        }
}

/* Hitscan: return id of nearest ALIVE ZOMBIE whose body the shooter's
 * aim ray actually passes through, or -1. No PvP -- players only ever
 * damage zombies.
 *
 * This is a proper line-vs-circle test, not a fixed angular cone: the
 * "cone" version was forgiving at any distance and unrelated to how
 * big the zombie actually looks on screen. Here we project the
 * zombie's position onto the aim ray to find the closest approach
 * point, then check how far off that ray the zombie actually is
 * (perpendicular distance) against its real radius -- i.e. does the
 * crosshair genuinely overlap the model, the same way a hitscan
 * weapon works in any normal FPS.
 *
 * COUPLING WARNING: ZOMBIE_RADIUS should match (or be slightly
 * smaller than -- never larger than) Zombie.gd's CapsuleMesh radius,
 * or the hitbox won't match what's rendered on screen. */
#define ZOMBIE_RADIUS 0.35f

static int shoot_check(int shooter_id)
{
    ServerPlayer *sh = &g_players[shooter_id];
    f32 rx = cosf(sh->angle);
    f32 ry = sinf(sh->angle);
    f32 best = SHOOT_RANGE;   /* tracks closest qualifying hit, along the ray */
    int hit  = -1;
    int i;

    for (i = 0; i < MAX_ZOMBIES; i++) {
        if (!g_zombies[i].active || !g_zombies[i].alive) continue;
        if (g_zombies[i].session_id != sh->session_id) continue;
        f32 dx = g_zombies[i].x - sh->x;
        f32 dy = g_zombies[i].y - sh->y;

        /* proj = how far along the aim ray the zombie's closest
         * approach point is. Negative means it's behind the shooter. */
        f32 proj = dx*rx + dy*ry;
        if (proj <= 0.0f || proj >= best) continue;

        /* perp = how far off the ray (off-crosshair) the zombie
         * actually is at that closest approach point. */
        f32 dist_sq = dx*dx + dy*dy;
        f32 perp_sq = dist_sq - proj*proj;
        if (perp_sq < 0.0f) perp_sq = 0.0f;   /* guard tiny float error */
        if (perp_sq > ZOMBIE_RADIUS * ZOMBIE_RADIUS) continue;

        best = proj;
        hit  = i;
    }
    return hit;
}

/* Classify an ALREADY-CONFIRMED hit (from shoot_check) as head or
 * body, using the shooter's vertical aim and horizontal distance to
 * the target. This never changes WHETHER something got hit -- only
 * whether it counts as a headshot -- so shoot_check's existing
 * horizontal targeting/miss behavior is completely untouched. */
static int classify_headshot(int shooter_id, int zombie_id)
{
    ServerPlayer *sh = &g_players[shooter_id];
    ServerZombie *z  = &g_zombies[zombie_id];
    f32 dx   = z->x - sh->x;
    f32 dy   = z->y - sh->y;
    f32 dist = sqrtf(dx*dx + dy*dy);

    /* Where a straight line at the shooter's pitch crosses the
     * target's vertical column, relative to the floor. */
    f32 hit_y = PLAYER_EYE_HEIGHT + dist * tanf(sh->inp_pitch);
    if (hit_y < 0.0f)          hit_y = 0.0f;
    if (hit_y > ZOMBIE_HEIGHT) hit_y = ZOMBIE_HEIGHT;

    f32 head_threshold = ZOMBIE_HEIGHT * (1.0f - HEAD_ZONE_FRACTION);
    return (hit_y >= head_threshold) ? 1 : 0;
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

static void send_state_to_session(u8 session_id)
{
    PktState pkt;
    int      active = 0, zactive = 0, i;
    memset(&pkt, 0, sizeof(pkt));
    pkt.hdr.type = PKT_STATE;
    pkt.hdr.tick = g_tick;

    for (i = 0; i < NET_MAX_PLAYERS; i++) {
        if (!g_players[i].active || g_players[i].session_id != session_id) continue;
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
        if (!g_zombies[i].active || g_zombies[i].session_id != session_id) continue;
        ZombieState *zs = &pkt.zombies[zactive++];
        zs->zombie_id = (u8)i;
        zs->alive     = (u8)g_zombies[i].alive;
        zs->x         = g_zombies[i].x;
        zs->y         = g_zombies[i].y;
        zs->health    = g_zombies[i].health;
    }
    pkt.zombie_count = (u8)zactive;
    pkt.wave         = g_wave[session_id];

    {
        int pc;
        for (pc = 0; pc < g_pickup_spawn_count; pc++) {
            PickupState *ps = &pkt.pickups[pc];
            ps->pickup_id = (u8)pc;
            ps->type      = g_pickup_spawn_type[pc];
            ps->active    = (u8)g_pickups[session_id][pc].active;
            ps->x         = g_pickup_spawn_x[pc];
            ps->y         = g_pickup_spawn_y[pc];
        }
        pkt.pickup_count = (u8)g_pickup_spawn_count;
    }

    for (i = 0; i < NET_MAX_PLAYERS; i++)
        if (g_players[i].active && g_players[i].session_id == session_id)
            send_to(i, &pkt, sizeof(pkt));
}

/* One PktState per active session, instead of one global broadcast --
 * this is the actual fix for solo/coop bleeding into each other:
 * players in different sessions now get entirely separate snapshots. */
static void broadcast_state(void)
{
    u8 s;
    for (s = 0; s < SESSION_COUNT; s++)
        if (any_player_connected_in(s))
            send_state_to_session(s);
}

static void broadcast_hit(u8 session_id, u8 victim_id, u8 victim_type, u8 attacker_id, u8 damage, u8 headshot)
{
    PktHit pkt;
    int i;
    memset(&pkt, 0, sizeof(pkt));
    pkt.hdr.type     = PKT_HIT;
    pkt.hdr.tick     = g_tick;
    pkt.victim_id    = victim_id;
    pkt.victim_type  = victim_type;
    pkt.attacker_id  = attacker_id;
    pkt.damage       = damage;
    pkt.headshot     = headshot;
    for (i = 0; i < NET_MAX_PLAYERS; i++)
        if (g_players[i].active && g_players[i].session_id == session_id)
            send_to(i, &pkt, sizeof(pkt));
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
            if (n < (ssize_t)sizeof(PktConnect)) break;
            PktConnect *c = (PktConnect *)buf;
            int id = find_player(&from);
            if (id < 0) {
                id = alloc_player(&from, c->mode);
                if (id < 0) { fprintf(stderr, "Server full\n"); break; }
                u8 sid = g_players[id].session_id;
                printf("Player %d connected (session %d, %s)  %s:%d\n",
                       id, sid, c->mode == CONNECT_MODE_SOLO ? "solo" : "coop",
                       inet_ntoa(from.sin_addr), ntohs(from.sin_port));
                if (count_alive_zombies_in(sid) == 0)
                    spawn_wave(sid);
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
            g_players[id].inp_pitch      = inp->pitch;
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

        /* find nearest alive player in the SAME session */
        int target = -1;
        f32 best_dist = 1e9f;
        for (p = 0; p < NET_MAX_PLAYERS; p++) {
            if (!g_players[p].active || !g_players[p].alive) continue;
            if (g_players[p].session_id != z->session_id) continue;
            f32 dx = g_players[p].x - z->x;
            f32 dy = g_players[p].y - z->y;
            f32 d  = sqrtf(dx*dx + dy*dy);
            if (d < best_dist) { best_dist = d; target = p; }
        }
        if (target < 0) continue;   /* nobody alive to chase in this session */

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
            broadcast_hit(z->session_id, (u8)target, ENTITY_PLAYER, ATTACKER_ZOMBIE, dmg, 0);
            if (pl->health == 0) {
                pl->alive         = 0;
                pl->respawn_timer = RESPAWN_TICKS;
                printf("Player %d was overrun by zombies\n", target);
            }
            z->attack_cooldown = ZOMBIE_ATTACK_COOLDOWN;
        }
        if (z->attack_cooldown > 0) z->attack_cooldown--;
    }

    /* Next wave once the current session's zombies are fully cleared
     * (and someone in that session is still playing) */
    {
        u8 s;
        for (s = 0; s < SESSION_COUNT; s++)
            if (any_player_connected_in(s) && count_alive_zombies_in(s) == 0)
                spawn_wave(s);
    }
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

        pickup_tick_for_player(p);

        /* Rising-edge shoot -- zombies only, no PvP */
        if (p->inp_shoot && !p->inp_shoot_prev && p->ammo > 0) {
            p->ammo--;
            int hit = shoot_check(i);
            if (hit >= 0) {
                ServerZombie *z    = &g_zombies[hit];
                int headshot       = classify_headshot(i, hit);
                u32 raw_dmg        = headshot ? HEADSHOT_DAMAGE : SHOOT_DAMAGE;
                u8  dmg            = (raw_dmg > 255u) ? 255u : (u8)raw_dmg;  /* wire field is one byte */

                z->health = (z->health > dmg) ? z->health - dmg : 0;
                broadcast_hit(p->session_id, (u8)hit, ENTITY_ZOMBIE, (u8)i, dmg, (u8)headshot);
                if (z->health == 0) {
                    z->alive  = 0;
                    z->active = 0;
                    printf("Zombie %d killed by player %d%s\n",
                           hit, i, headshot ? " (HEADSHOT)" : "");
                }
            }
        }
        p->inp_shoot_prev = p->inp_shoot;
    }

    zombie_tick();
    pickup_respawn_tick();
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
    init_pickups();

    g_sock = net_udp_socket();
    if (g_sock < 0) return 1;
    if (net_bind(g_sock, port) < 0) return 1;

    printf("doom-qnx-coop server  port=%d  tick_rate=%d Hz\n",
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
