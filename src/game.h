#pragma once

#include <stdbool.h>

#define CUBE_SIZE 0.4f

#define MAX_BOTS        512
#define MAX_PROJECTILES 2048
#define TOTAL_SCRIPTS     7
#define LLM_SCRIPT_IDX    6
#define MAX_SCAN_HITS    64

typedef enum {
    WEAPON_MACHINE_GUN = 0,
    WEAPON_AUTO_CANNON = 1,
    WEAPON_LASER       = 2
} WeaponType;

typedef struct {
    WeaponType left_weapon;
    WeaponType right_weapon;
    int        armour;       /* 0-3 */
    float      max_hp;
    float      max_speed;
    float      body_scale;
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
    int   scan_hit_count;
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
    struct lua_State *L;
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
