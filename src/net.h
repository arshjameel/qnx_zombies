#ifndef NET_H
#define NET_H

#include "common.h"
#include "map.h"   

/* ------------------------------------------------------------------ */
/* Protocol constants                                                 */
/* ------------------------------------------------------------------ */
#define NET_PORT         7777
#define NET_MAX_PLAYERS  8
#define NET_TICK_RATE    20        
#define MAX_ZOMBIES      16

/* ------------------------------------------------------------------ */
/* Packet types                                                       */
/* ------------------------------------------------------------------ */
#define PKT_CONNECT        0x01u  
#define PKT_ACCEPT         0x02u  
#define PKT_INPUT          0x03u  
#define PKT_STATE          0x04u  
#define PKT_HIT            0x06u  
#define PKT_DISCONNECT     0x07u  
#define PKT_RESET_SESSION  0x08u

/* ------------------------------------------------------------------ */
/* Entity types                                                       */
/* ------------------------------------------------------------------ */
#define ENTITY_PLAYER   0u
#define ENTITY_ZOMBIE   1u

/* ------------------------------------------------------------------ */
/* Zombie types                                                       */
/* ------------------------------------------------------------------ */
#define ZOMBIE_TYPE_NORMAL  0u
#define ZOMBIE_TYPE_TANK    1u
#define ZOMBIE_TYPE_BOSS    2u

/* ------------------------------------------------------------------ */
/* Pickup types                                                       */
/* ------------------------------------------------------------------ */
#define PICKUP_AMMO     0u
#define PICKUP_HEALTH   1u
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
    u8        mode;      /* 0 = coop, 1 = solo */
} PktConnect;                                          

#define CONNECT_MODE_COOP  0u
#define CONNECT_MODE_SOLO  1u

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
    f32       look_angle;    
    u8        shoot;
    f32       pitch;         
    u8        shoot_auto;
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
    u8  type;   
    f32 x;
    f32 y;
    f32 z;      
    u8  health;
    u8  health_max;   
} ZombieState;                                         

typedef struct {
    u8  pickup_id;    
    u8  type;         
    u8  active;       
    f32 x;
    f32 y;
} PickupState;                                         

typedef struct {
    PktHeader   hdr;
    u8          player_count;
    PlayerState players[NET_MAX_PLAYERS];
    u8          zombie_count;
    ZombieState zombies[MAX_ZOMBIES];
    u8          wave;
    u8          pickup_count;
    PickupState pickups[MAX_PICKUP_SPAWNS];
} PktState;                    

typedef struct {
    PktHeader hdr;
    u8        victim_id;
    u8        victim_type;   
    u8        attacker_id;
    u8        damage;
    u8        headshot;      
} PktHit;                                              

#pragma pack(pop)

/* ------------------------------------------------------------------ */
/* Socket helpers                                                     */
/* ------------------------------------------------------------------ */

int net_udp_socket(void);
int net_bind(int sock, u16 port);

#endif 
