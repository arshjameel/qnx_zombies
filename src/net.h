#ifndef NET_H
#define NET_H

#include "common.h"

/* any changed here must mirror the changes in Network.gd's constants 
 * and functions
 */

/* ------------------------------------------------------------------ */
#define NET_PORT         7777
#define NET_MAX_PLAYERS  8
#define NET_TICK_RATE    20        /* server sends state 20x/sec */
#define MAX_ZOMBIES      16

/* ------------------------------------------------------------------ */
/* Packet types                                                       */
/* ------------------------------------------------------------------ */
#define PKT_CONNECT     0x01u  /* client -> server: join request       */
#define PKT_ACCEPT      0x02u  /* server -> client: assigned player_id */
#define PKT_INPUT       0x03u  /* client -> server: player inputs      */
#define PKT_STATE       0x04u  /* server -> client: full game state    */
#define PKT_HIT         0x06u  /* server -> client: hit notification   */
#define PKT_DISCONNECT  0x07u  /* either direction                     */

/* ------------------------------------------------------------------ */
/* Entity types (used in PktHit to disambiguate victim_id)            */
/* ------------------------------------------------------------------ */
#define ENTITY_PLAYER   0u
#define ENTITY_ZOMBIE   1u

/* Sentinel attacker_id */
#define ATTACKER_ZOMBIE 0xFFu

/* ------------------------------------------------------------------ */
/* Packet structures                                                  */
/* ------------------------------------------------------------------ */
#pragma pack(push, 1)

typedef struct {
    u8  type;
    u8  player_id;
    u32 tick;
} PktHeader;

typedef struct {
    PktHeader hdr;
    u8        assigned_id;
    f32       spawn_x;
    f32       spawn_y;
    f32       spawn_angle;
} PktAccept;

typedef struct {
    PktHeader hdr;
    u8        forward;
    u8        back;
    u8        strafe_left;
    u8        strafe_right;
    f32       look_angle; // absolute yaw in radians for client mouse-look
    u8        shoot;
} PktInput;

typedef struct {
    u8  player_id;
    u8  alive;
    f32 x;
    f32 y;
    f32 angle;
    u8  health;
    u8  ammo;
} PlayerState;

typedef struct {
    u8  zombie_id;
    u8  alive;
    f32 x;
    f32 y;
    u8  health;
} ZombieState;

typedef struct {
    PktHeader   hdr;
    u8          player_count;
    PlayerState players[NET_MAX_PLAYERS];
    u8          zombie_count;
    ZombieState zombies[MAX_ZOMBIES];
    u8          wave;
} PktState;

typedef struct {
    PktHeader hdr;
    u8        victim_id;
    u8        victim_type;   /* ENTITY_PLAYER or ENTITY_ZOMBIE */
    u8        attacker_id;   /* player id or ATTACKER_ZOMBIE  */
    u8        damage;
} PktHit;                                              

#pragma pack(pop)

/* ------------------------------------------------------------------ */
/* Socket helpers                                                       */
/* ------------------------------------------------------------------ */

/* create non-blocking UDP socket */
int net_udp_socket(void);

/* bind socket to port/server */
int net_bind(int sock, u16 port);

#endif
