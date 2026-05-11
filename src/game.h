#pragma once

#include <stdbool.h>
#include <Python.h>

#define CUBE_SIZE 0.4f

#define MAX_BOTS        512
#define MAX_PROJECTILES 2048
#define TOTAL_SCRIPTS     7
#define LLM_SCRIPT_IDX    6
#define MAX_SCAN_HITS    64
#define MAX_WEAPONS       4

typedef enum {
    WEAPON_MACHINE_GUN = 0,
    WEAPON_AUTO_CANNON = 1,
    WEAPON_LASER       = 2
} WeaponType;

typedef enum {
    LOCO_WHEELS = 0,
    LOCO_TRACKS = 1,
    LOCO_LEGS_4 = 2,
    LOCO_LEGS_2 = 3
} Locomotion;

typedef enum {
    BODY_CUBE     = 0,
    BODY_TALL     = 1,
    BODY_FLAT     = 2,
    BODY_LONG_LOW = 3,
    BODY_TOWER    = 4,
    BODY_WEDGE    = 5,
    BODY_TANK     = 6
} BodyShape;

typedef enum {
    MOUNT_LEFT      = 0,
    MOUNT_RIGHT     = 1,
    MOUNT_TOP       = 2,
    MOUNT_TOP_FRONT = 3,
    MOUNT_TOP_REAR  = 4
} WeaponMount;

typedef struct {
    WeaponType  type;
    WeaponMount mount;
} WeaponSlot;

typedef struct {
    Locomotion locomotion;
    BodyShape  body;
    int        weapon_count;
    WeaponSlot weapons[MAX_WEAPONS];
    float      max_hp;
    float      max_speed;
    float      turn_rate;     /* body angular speed in rad/s */
    float      body_sx;       /* width  scale (relative to CUBE_SIZE) */
    float      body_sy;       /* height scale */
    float      body_sz;       /* depth  scale */
    float      total_weight;  /* body + weapons (for diagnostics) */
    int        script_idx;
} BotConfig;

typedef struct {
    float body_angle;
    float turret_angle;
    float desired_body_angle;
    float desired_turret_angle;
    int   move_requested;
    float scan_hit_x[MAX_SCAN_HITS];
    float scan_hit_z[MAX_SCAN_HITS];
    int   scan_hit_type[MAX_SCAN_HITS]; /* 0 = bot, 1 = wall */
    int   scan_hit_count;
    float weapon_cd[MAX_WEAPONS]; /* per-weapon fire cooldown */
    float move_anim_t;            /* phase advanced by speed; drives leg/wheel anim */
} BotInertia;

typedef struct {
    bool    active;
    float   x, y, z;          /* position */
    float   vx, vy, vz;       /* velocity */
    unsigned char r, g, b, a;  /* color */
    float   hp;
    int     script_id;         /* team / script slot index */
    BotConfig  config;
    BotInertia inertia;
    PyObject  *py_ns;          /* Python namespace dict for this bot's script */
} Bot;

typedef struct {
    bool       active;
    float      x, y, z;           /* position */
    unsigned char r, g, b, a;     /* color */
    int        owner_idx;          /* index into g_bots[] */
    int        owner_script;       /* Script slot of the owner (for friendly-fire) */
    WeaponType weapon_type;
    float      lifetime;
    float      dir_x, dir_z;
    float      speed;
    float      damage;
} Proj;

extern Bot  g_bots[MAX_BOTS];
extern int  g_bot_count;
extern Proj g_projs[MAX_PROJECTILES];
extern int  g_proj_count;
