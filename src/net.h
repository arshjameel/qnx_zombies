#ifndef NET_H
#define NET_H

#include "common.h"
#include "map.h"   /* for MAX_PICKUP_SPAWNS -- pickup slots are 1:1 with map markers */

/* ------------------------------------------------------------------ */
/* Protocol constants                                                   */
/*                                                                      */
/* IMPORTANT: this file is the source of truth for the wire protocol.  */
/* The Godot client (godot_client/scripts/Network.gd) re-implements    */
/* every one of these structs by hand in GDScript since GDScript can't */
/* #include a C header. If you change anything here, mirror the change */
/* in Network.gd's PACK_* constants and its read/write helper funcs,  */
/* or the client and server will silently desync.                    */
/* ------------------------------------------------------------------ */
#define NET_PORT         7777
#define NET_MAX_PLAYERS  8
#define NET_TICK_RATE    20        /* server sends state 20x/sec */
#define MAX_ZOMBIES      16

/* ------------------------------------------------------------------ */
/* Packet types                                                         */
/* ------------------------------------------------------------------ */
#define PKT_CONNECT     0x01u  /* client -> server: join request       */
#define PKT_ACCEPT      0x02u  /* server -> client: assigned player_id */
#define PKT_INPUT       0x03u  /* client -> server: player inputs      */
#define PKT_STATE       0x04u  /* server -> client: full game state    */
#define PKT_HIT         0x06u  /* server -> client: hit notification   */
#define PKT_DISCONNECT  0x07u  /* either direction                     */
#define PKT_RESET_SESSION 0x08u  /* client -> server: replay after beating
                                   * wave 4 -- see server.c's handler for
                                   * exactly what gets reset. No extra
                                   * fields beyond the header; the sender's
                                   * session is found the same way
                                   * PKT_INPUT/PKT_DISCONNECT already do. */

/* ------------------------------------------------------------------ */
/* Entity types (used in PktHit to disambiguate victim_id)             */
/* ------------------------------------------------------------------ */
#define ENTITY_PLAYER   0u
#define ENTITY_ZOMBIE   1u

/* ------------------------------------------------------------------ */
/* Pickup types                                                         */
/* ------------------------------------------------------------------ */
#define PICKUP_AMMO     0u
#define PICKUP_HEALTH   1u

/* Sentinel attacker_id meaning "a zombie did this", since real player
 * ids only ever run 0..NET_MAX_PLAYERS-1 */
#define ATTACKER_ZOMBIE 0xFFu

/* ------------------------------------------------------------------ */
/* Packet structures (packed, little-endian)                           */
/* ------------------------------------------------------------------ */
#pragma pack(push, 1)

typedef struct {
    u8  type;
    u8  player_id;
    u32 tick;
} PktHeader;                                          /* 6 bytes */

typedef struct {
    PktHeader hdr;
    u8        mode;      /* 0 = coop (shared world), 1 = solo (private world) */
} PktConnect;                                          /* 7 bytes */

#define CONNECT_MODE_COOP  0u
#define CONNECT_MODE_SOLO  1u

typedef struct {
    PktHeader hdr;
    u8        assigned_id;
    f32       spawn_x;
    f32       spawn_y;
    f32       spawn_angle;
} PktAccept;                                           /* 19 bytes */

typedef struct {
    PktHeader hdr;
    u8        forward;
    u8        back;
    u8        strafe_left;
    u8        strafe_right;
    f32       look_angle;    /* absolute yaw in radians, client mouse-look */
    u8        shoot;
    f32       pitch;         /* absolute pitch in radians, +up/-down, for
                               * headshot vertical classification only --
                               * NOT used for horizontal targeting/miss */
} PktInput;                                            /* 19 bytes */

typedef struct {
    u8  player_id;
    u8  alive;
    f32 x;
    f32 y;
    f32 angle;
    u8  health;
    u8  ammo;
} PlayerState;                                         /* 16 bytes */

/* ------------------------------------------------------------------ */
/* Zombie types -- colors/sizes are a client-side concern (Zombie.gd /  */
/* qnx_client's build_entity_geometry), the server only cares about    */
/* the stat differences (health/speed/dmg, see server.c).              */
/* ------------------------------------------------------------------ */
#define ZOMBIE_TYPE_NORMAL  0u   /* green, wave 1+  */
#define ZOMBIE_TYPE_TANK    1u   /* red, bigger, wave 2+ -- the mini-boss */
#define ZOMBIE_TYPE_BOSS    2u   /* blue, biggest, wave 4 final boss */

typedef struct {
    u8  zombie_id;
    u8  alive;
    u8  type;   /* ZOMBIE_TYPE_NORMAL / TANK / BOSS -- picks color+size client-side */
    f32 x;
    f32 y;
    f32 z;      /* height above floor. 0 unless the server has it
                 * climbing a ramp or standing on a platform -- see
                 * server.c's zombie_tick() COUPLING WARNING. */
    u8  health;
    u8  health_max;  /* so the client's health-bar percentage is correct
                       * regardless of type -- tank/boss have far more HP
                       * than a normal zombie's fixed old value. */
} ZombieState;                                         /* 17 bytes */

typedef struct {
    u8  pickup_id;    /* index into the map's pickup-marker list, stable */
    u8  type;         /* PICKUP_AMMO or PICKUP_HEALTH */
    u8  active;       /* 0 while on cooldown after being taken */
    f32 x;
    f32 y;
} PickupState;                                         /* 11 bytes */

typedef struct {
    PktHeader   hdr;
    u8          player_count;
    PlayerState players[NET_MAX_PLAYERS];
    u8          zombie_count;
    ZombieState zombies[MAX_ZOMBIES];
    u8          wave;
    /* Pickups appended at the end on purpose -- keeps every existing
     * field's offset unchanged for anything still using the old layout. */
    u8          pickup_count;
    PickupState pickups[MAX_PICKUP_SPAWNS];
} PktState;                    /* 6+1+128+1+272+1+1+(16*11) = 586 bytes fixed --
                                 * verified with sizeof(), not hand math, since
                                 * ZombieState grew 15->17 bytes with the type/
                                 * health_max fields. */

typedef struct {
    PktHeader hdr;
    u8        victim_id;
    u8        victim_type;   /* ENTITY_PLAYER or ENTITY_ZOMBIE */
    u8        attacker_id;   /* player id, or ATTACKER_ZOMBIE  */
    u8        damage;
    u8        headshot;      /* 1 if this hit was in the zombie's head zone */
} PktHit;                                              /* 11 bytes */

#pragma pack(pop)

/* ------------------------------------------------------------------ */
/* Socket helpers                                                       */
/* ------------------------------------------------------------------ */

/* Create a non-blocking UDP socket. Returns fd >= 0 or -1. */
int net_udp_socket(void);

/* Bind socket to port (server). Returns 0 or -1. */
int net_bind(int sock, u16 port);

#endif /* NET_H */
