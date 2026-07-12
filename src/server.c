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

/* Per-type zombie stats. Health is capped at 255 on purpose -- the wire
 * field (ZombieState.health) is a single byte, so BOSS_HEALTH is the
 * max a u8 can hold rather than a "real" boss-scale number. */
#define ZOMBIE_SPEED   0.05f
#define ZOMBIE_HEALTH  100
#define ZOMBIE_DAMAGE  20

#define TANK_SPEED     0.04f   /* a little slower -- bulkier */
#define TANK_HEALTH    180
#define TANK_DAMAGE    35

#define BOSS_SPEED     0.035f
#define BOSS_HEALTH    255
#define BOSS_DAMAGE    50

#define ZOMBIE_ATTACK_RANGE       0.8f
#define ZOMBIE_ATTACK_COOLDOWN    (NET_TICK_RATE * 1)  /* 1 attack/sec */

/* ------------------------------------------------------------------ */
/* Wave design -- 4 waves, all on the one map (no separate arena):     */
/*   Wave 1: 3 normal                                                   */
/*   Wave 2: 5 normal + 1 tank (mini-boss)                              */
/*   Wave 3: 7 normal + 3 tank                                          */
/*   Wave 4: final boss                                                 */
/* spawn_wave() is a no-op past WAVE_COUNT -- wave 4's boss is the last */
/* thing that ever spawns for a session; no wave 5 follows.             */
/* ------------------------------------------------------------------ */
#define WAVE_COUNT 4

typedef struct {
    int normal_count;
    int tank_count;
    int boss_count;
} WaveDef;

static const WaveDef WAVE_TABLE[WAVE_COUNT] = {
    { 3, 0, 0 },   /* wave 1 */
    { 5, 1, 0 },   /* wave 2 */
    { 7, 3, 0 },   /* wave 3 */
    { 0, 0, 1 },   /* wave 4 -- final boss */
};

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

/* Zombie ramp/platform climbing. This is a deliberate approximation,
 * not a full port of LevelBuilder.gd's per-tile ramp-chain math: the
 * server doesn't know which ramp tile is which step of which chain,
 * it only knows the raw tile type under a zombie's feet right now.
 * So target height is just "0 on floor, halfway up while anywhere on
 * a ramp tile, full height on a platform tile", eased toward smoothly
 * each tick rather than snapped -- looks like climbing/dropping in
 * practice even though it doesn't trace the exact ramp slope the way
 * the player's real physics-based climb does.
 *
 * COUPLING WARNING: PLATFORM_HEIGHT must match LevelBuilder.gd's
 * PLATFORM_HEIGHT, or a zombie will visually stand at the wrong
 * height relative to the platform mesh the client actually drew. */
#define PLATFORM_HEIGHT   2.0f
#define RAMP_MID_HEIGHT   (PLATFORM_HEIGHT * 0.5f)
/* COUPLING: mirrors qnx_client's CLIMB_SPEED_UP/DOWN in main.c, so
 * zombies feel the same "weight" as the player -- falling faster
 * than rising, rather than floating at a uniform rate either way. */
#define ZOMBIE_CLIMB_SPEED_UP   3.0f
#define ZOMBIE_CLIMB_SPEED_DOWN 8.0f

#define SPAWN_COUNT 4
static const f32 SPAWN_X[SPAWN_COUNT] = { 2.5f, 21.5f,  2.5f, 21.5f };
static const f32 SPAWN_Y[SPAWN_COUNT] = { 2.5f,  2.5f, 21.5f, 21.5f };
static const f32 SPAWN_A[SPAWN_COUNT] = { 0.0f,  3.14f,  1.57f, 4.71f };

/* Zombie spawn points -- all verified open floor tiles in map.c,
 * spread across the middle of the map, away from player spawn corners.
 * Index 3 was originally (12.5, 11.5), which the map redesign moved
 * onto the elevated platform -- fixed to (12.5, 6.5), still open floor,
 * still roughly central. */
#define ZOMBIE_SPAWN_COUNT 8
static const f32 ZOMBIE_SPAWN_X[ZOMBIE_SPAWN_COUNT] =
    { 8.5f, 15.5f, 4.5f, 12.5f, 19.5f, 8.5f, 15.5f, 12.5f };
static const f32 ZOMBIE_SPAWN_Y[ZOMBIE_SPAWN_COUNT] =
    { 2.5f,  2.5f, 11.5f,  6.5f, 11.5f, 21.5f, 21.5f, 20.5f };

/* Dedicated boss entrance point -- north-center, clearly clear of the
 * platform/ramp structure and away from every player spawn corner. */
#define BOSS_SPAWN_X 12.5f
#define BOSS_SPAWN_Y 3.5f

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
    u8  type;           /* ZOMBIE_TYPE_NORMAL / TANK / BOSS */
    f32 x, y;
    f32 z;              /* height above floor -- see zombie_tick's COUPLING WARNING */
    u8  health;
    u8  health_max;      /* set at spawn from the per-type constant, sent on the wire
                           * so the client's health bar percentage is correct */
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

/* Finds a free zombie slot and fills it in as `type` at (x,y). Health
 * (current + max) comes from the per-type constants so tank/boss are
 * meaningfully tankier than a normal zombie. Returns 1 if a slot was
 * found and used, 0 if MAX_ZOMBIES is already full. */
static int spawn_one_zombie(u8 session_id, u8 type, f32 x, f32 y)
{
    int i;
    for (i = 0; i < MAX_ZOMBIES; i++) {
        u8 hp;
        if (g_zombies[i].active) continue;
        switch (type) {
            case ZOMBIE_TYPE_TANK: hp = TANK_HEALTH; break;
            case ZOMBIE_TYPE_BOSS: hp = BOSS_HEALTH; break;
            default:               hp = ZOMBIE_HEALTH; break;
        }
        g_zombies[i].active          = 1;
        g_zombies[i].alive           = 1;
        g_zombies[i].session_id      = session_id;
        g_zombies[i].type            = type;
        g_zombies[i].z               = 0.0f;
        g_zombies[i].health          = hp;
        g_zombies[i].health_max      = hp;
        g_zombies[i].x               = x;
        g_zombies[i].y               = y;
        g_zombies[i].attack_cooldown = 0;
        return 1;
    }
    return 0;
}

/* Spawns the next wave for a session per WAVE_TABLE. No-op past
 * WAVE_COUNT -- the session has already beaten the boss and no
 * further wave follows. */
static void spawn_wave(u8 session_id)
{
    u8 w = g_wave[session_id] + 1;
    int spawned = 0, i;
    const WaveDef *def;

    if (w > WAVE_COUNT) return;   /* boss already defeated -- session is over */
    g_wave[session_id] = w;
    def = &WAVE_TABLE[w - 1];

    for (i = 0; i < def->normal_count; i++) {
        f32 x = ZOMBIE_SPAWN_X[i % ZOMBIE_SPAWN_COUNT];
        f32 y = ZOMBIE_SPAWN_Y[i % ZOMBIE_SPAWN_COUNT];
        if (spawn_one_zombie(session_id, ZOMBIE_TYPE_NORMAL, x, y)) spawned++;
    }
    for (i = 0; i < def->tank_count; i++) {
        /* offset the index so tanks don't land on the exact same tiles
         * as the normals that were just placed above */
        int idx = i + def->normal_count;
        f32 x = ZOMBIE_SPAWN_X[idx % ZOMBIE_SPAWN_COUNT];
        f32 y = ZOMBIE_SPAWN_Y[idx % ZOMBIE_SPAWN_COUNT];
        if (spawn_one_zombie(session_id, ZOMBIE_TYPE_TANK, x, y)) spawned++;
    }
    for (i = 0; i < def->boss_count; i++) {
        if (spawn_one_zombie(session_id, ZOMBIE_TYPE_BOSS, BOSS_SPAWN_X, BOSS_SPAWN_Y)) spawned++;
    }

    printf("Session %d: wave %d started -- %d normal, %d tank, %d boss (%d total)\n",
           session_id, w, def->normal_count, def->tank_count, def->boss_count, spawned);
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
        zs->zombie_id  = (u8)i;
        zs->alive      = (u8)g_zombies[i].alive;
        zs->type       = g_zombies[i].type;
        zs->x          = g_zombies[i].x;
        zs->y          = g_zombies[i].y;
        zs->z          = g_zombies[i].z;
        zs->health     = g_zombies[i].health;
        zs->health_max = g_zombies[i].health_max;
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

/* Full reset of a session for "replay" after beating wave 4 -- wave
 * counter, every zombie belonging to the session, every pickup's
 * cooldown, and every currently-connected player's health/ammo/
 * position in that session (in coop, one player choosing replay
 * restarts it for the whole team, matching a shared-world session's
 * "we all just beat it together" framing). Ends by calling
 * spawn_wave() directly rather than relying on the per-tick "zombie
 * count reached zero" check to notice -- immediate feels right for
 * an explicit player action, and mirrors how PKT_CONNECT already
 * kicks off wave 1 immediately rather than waiting a tick. */
static void reset_session(u8 sid)
{
    int i;

    g_wave[sid] = 0;

    for (i = 0; i < MAX_ZOMBIES; i++) {
        if (g_zombies[i].active && g_zombies[i].session_id == sid)
            g_zombies[i].active = 0;
    }

    for (i = 0; i < g_pickup_spawn_count; i++) {
        g_pickups[sid][i].active        = 1;
        g_pickups[sid][i].respawn_timer = 0;
    }

    for (i = 0; i < NET_MAX_PLAYERS; i++) {
        if (!g_players[i].active || g_players[i].session_id != sid) continue;
        g_players[i].x              = SPAWN_X[i % SPAWN_COUNT];
        g_players[i].y              = SPAWN_Y[i % SPAWN_COUNT];
        g_players[i].angle          = SPAWN_A[i % SPAWN_COUNT];
        g_players[i].inp_look_angle = g_players[i].angle;
        g_players[i].health         = 100;
        g_players[i].ammo           = AMMO_MAX;
        g_players[i].alive          = 1;
        g_players[i].respawn_timer  = 0;
    }

    spawn_wave(sid);
    printf("Session %d: reset for replay\n", sid);
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
                /* Only auto-start wave 1 for a session that hasn't
                 * played at all yet. Without the g_wave==0 guard, a
                 * player reconnecting mid-run (or a second coop player
                 * joining between waves) could re-trigger spawn_wave()
                 * on top of whatever wave is already in progress --
                 * spawn_wave() isn't idempotent (it always increments
                 * g_wave), so this could silently double-advance the
                 * wave counter if it fires the same tick as the normal
                 * wave-clear check below. */
                if (g_wave[sid] == 0)
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

        case PKT_RESET_SESSION: {
            int id = find_player(&from);
            if (id >= 0) reset_session(g_players[id].session_id);
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

/* Moves z toward whatever height the tile under the zombie's feet
 * implies, at asymmetric rates (falling faster than climbing, for a
 * "weight" feel) -- see the ZOMBIE_CLIMB_SPEED_UP/DOWN COUPLING
 * WARNING above for why this is an approximation rather than a true
 * ramp-slope trace. */
static void zombie_height_tick(ServerZombie *z)
{
    int tile = map_tile((int)z->x, (int)z->y);
    f32 target_z;
    f32 step_up, step_down;

    if (tile == TILE_PLATFORM)      target_z = PLATFORM_HEIGHT;
    else if (tile == TILE_RAMP)     target_z = RAMP_MID_HEIGHT;
    else                            target_z = 0.0f;

    step_up   = ZOMBIE_CLIMB_SPEED_UP   / (f32)NET_TICK_RATE;
    step_down = ZOMBIE_CLIMB_SPEED_DOWN / (f32)NET_TICK_RATE;
    if (z->z < target_z) {
        z->z += step_up;
        if (z->z > target_z) z->z = target_z;
    } else if (z->z > target_z) {
        z->z -= step_down;
        if (z->z < target_z) z->z = target_z;
    }
}

/* Per-type movement speed and attack damage -- see the ZOMBIE_SPEED /
 * TANK_SPEED / BOSS_SPEED and *_DAMAGE constants near the wave table. */
static f32 zombie_speed_for(u8 type)
{
    switch (type) {
        case ZOMBIE_TYPE_TANK: return TANK_SPEED;
        case ZOMBIE_TYPE_BOSS: return BOSS_SPEED;
        default:               return ZOMBIE_SPEED;
    }
}

static u8 zombie_damage_for(u8 type)
{
    switch (type) {
        case ZOMBIE_TYPE_TANK: return TANK_DAMAGE;
        case ZOMBIE_TYPE_BOSS: return BOSS_DAMAGE;
        default:               return ZOMBIE_DAMAGE;
    }
}

static void zombie_tick(void)
{
    int i, p;
    for (i = 0; i < MAX_ZOMBIES; i++) {
        ServerZombie *z = &g_zombies[i];
        if (!z->active || !z->alive) continue;

        zombie_height_tick(z);

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
            f32 speed = zombie_speed_for(z->type);
            f32 nx = z->x + (dx / dist) * speed;
            f32 ny = z->y + (dy / dist) * speed;
            if (!map_is_wall((int)nx, (int)z->y)) z->x = nx;
            if (!map_is_wall((int)z->x, (int)ny)) z->y = ny;
        } else if (z->attack_cooldown <= 0) {
            ServerPlayer *pl = &g_players[target];
            u8 dmg = zombie_damage_for(z->type);
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
