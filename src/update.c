#include "update.h"
#include "walls.h"
#include "fx.h"

#include "lua.h"
#include "lauxlib.h"

#include <math.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define HIT_RADIUS2      0.36f   /* squared hit radius = 0.6^2 */
#define BOT_WALL_MARGIN  0.35f   /* keep bots away from border wall inner face */

static float g_arena_half_x = 10.0f;
static float g_arena_half_z = 10.0f;

static float g_llm_damage = 0.0f;
static int   g_llm_kills  = 0;

static LlmTelemetry g_tel;

/* Last runtime error seen from an LLM bot, for LLM feedback */
static char  g_last_runtime_error[512] = {0};

void update_telemetry_reset(void) { memset(&g_tel, 0, sizeof(g_tel)); }
void update_telemetry_get(LlmTelemetry *out) { *out = g_tel; }
void update_telemetry_inc_fire_frame(void)   { g_tel.fire_frames++; }
void update_telemetry_inc_shots_fired(int n) { g_tel.shots_fired += n; }
void update_telemetry_inc_shots_hit(void)    { g_tel.shots_hit++; }
void update_telemetry_inc_think_frame(bool enemy_visible, float nearest_dist) {
    g_tel.think_frames++;
    if (enemy_visible) {
        g_tel.enemy_visible_frames++;
        g_tel.nearest_dist_sum += nearest_dist;
        g_tel.nearest_dist_samples++;
    }
}
void update_telemetry_inc_arena_bump(void) { g_tel.arena_bumps++; }
void update_telemetry_inc_wall_bump(void)  { g_tel.wall_bumps++; }

/* Deduplicate noisy per-frame script errors: only print when the message
 * changes for a given bot slot. */
#define MAX_BOTS_DEDUP 16
static char s_last_script_err[MAX_BOTS_DEDUP][256];

void update_reset_llm_stats(void) {
    g_llm_damage = 0.0f;
    g_llm_kills  = 0;
}

void update_get_llm_stats(float *dmg_out, int *kills_out) {
    *dmg_out   = g_llm_damage;
    *kills_out = g_llm_kills;
}

void update_clear_runtime_error(void) {
    g_last_runtime_error[0] = '\0';
}

void update_get_runtime_error(char *buf, int size) {
    strncpy(buf, g_last_runtime_error, (size_t)(size - 1));
    buf[size - 1] = '\0';
}

void update_set_arena(float half_x, float half_z) {
    g_arena_half_x = half_x;
    g_arena_half_z = half_z;
}

/* ----------------------------------------------------------------------- */

static const float BODY_TURN_RATE[4]   = { 10.0f, 6.0f, 3.5f, 2.0f };
static const float TURRET_TURN_RATE[3] = {  8.0f, 4.0f, 2.0f };

static float angle_step(float current, float desired, float rate, float dt) {
    float diff = desired - current;
    while (diff >  (float)M_PI) diff -= 2.0f * (float)M_PI;
    while (diff < -(float)M_PI) diff += 2.0f * (float)M_PI;
    float step = rate * dt;
    if (diff >  step) return current + step;
    if (diff < -step) return current - step;
    return desired;
}

/* ----------------------------------------------------------------------- */

void update_scripts(Bot *bots, int count, float dt) {
    for (int i = 0; i < count; i++) {
        Bot *b = &bots[i];
        if (!b->active || !b->L) continue;

        lua_State *L = b->L;

        lua_pushinteger(L, (lua_Integer)i);
        lua_setglobal(L, "__bot_idx");
        lua_pushnumber(L, (double)b->x);      lua_setglobal(L, "self_x");
        lua_pushnumber(L, (double)b->z);       lua_setglobal(L, "self_z");
        lua_pushinteger(L, b->script_id);      lua_setglobal(L, "self_team");
        lua_pushnumber(L, (double)b->hp);      lua_setglobal(L, "self_hp");
        lua_pushnumber(L, (double)b->config.max_hp); lua_setglobal(L, "self_max_hp");

        lua_getglobal(L, "think");
        if (lua_type(L, -1) != LUA_TFUNCTION) {
            lua_pop(L, 1);
            continue;
        }
        lua_pushnumber(L, (double)dt);
        if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
            const char *err = lua_tostring(L, -1);
            if (err == NULL) err = "(unknown error)";
            if (i < MAX_BOTS_DEDUP) {
                if (strncmp(s_last_script_err[i], err, 255) != 0) {
                    fprintf(stderr, "[script] bot %d: %s\n", i, err);
                    strncpy(s_last_script_err[i], err, 255);
                    s_last_script_err[i][255] = '\0';
                    /* Capture first unique runtime error from LLM bots for prompt feedback */
                    if (b->script_id == LLM_SCRIPT_IDX && g_last_runtime_error[0] == '\0') {
                        strncpy(g_last_runtime_error, err, sizeof(g_last_runtime_error) - 1);
                        g_last_runtime_error[sizeof(g_last_runtime_error) - 1] = '\0';
                    }
                }
            } else {
                fprintf(stderr, "[script] bot %d: %s\n", i, err);
            }
            lua_pop(L, 1);
        } else if (i < MAX_BOTS_DEDUP && s_last_script_err[i][0] != '\0') {
            /* Script recovered — reset so the next error is printed */
            s_last_script_err[i][0] = '\0';
        }
    }
}

void update_inertia(Bot *bots, int count, float dt) {
    for (int i = 0; i < count; i++) {
        Bot *b = &bots[i];
        if (!b->active) continue;

        int armour = b->config.armour;
        if (armour < 0) armour = 0;
        if (armour > 3) armour = 3;

        float brate = BODY_TURN_RATE[armour];
        b->inertia.body_angle = angle_step(b->inertia.body_angle,
                                           b->inertia.desired_body_angle, brate, dt);

        float trate_l = TURRET_TURN_RATE[(int)b->config.left_weapon];
        float trate_r = TURRET_TURN_RATE[(int)b->config.right_weapon];
        float trate   = (trate_l < trate_r) ? trate_l : trate_r;
        b->inertia.turret_angle = angle_step(b->inertia.turret_angle,
                                             b->inertia.desired_turret_angle, trate, dt);

        if (b->inertia.move_requested) {
            b->vx = cosf(b->inertia.body_angle) * b->config.max_speed;
            b->vz = sinf(b->inertia.body_angle) * b->config.max_speed;
            b->inertia.move_requested = 0;
        } else {
            b->vx = 0.0f;
            b->vz = 0.0f;
        }

        if (b->inertia.left_fire_cd  > 0.0f) b->inertia.left_fire_cd  -= dt;
        if (b->inertia.right_fire_cd > 0.0f) b->inertia.right_fire_cd -= dt;
    }
}

void update_movement(Bot *bots, int count, float dt) {
    for (int i = 0; i < count; i++) {
        Bot *b = &bots[i];
        if (!b->active) continue;

        b->x += b->vx * dt;
        b->z += b->vz * dt;

        bool is_llm = (b->script_id == LLM_SCRIPT_IDX);
        float bx = g_arena_half_x - BOT_WALL_MARGIN;
        float bz = g_arena_half_z - BOT_WALL_MARGIN;
        bool bumped_arena = false;
        if (b->x >  bx) { b->x =  bx; b->vx = -b->vx; bumped_arena = true; }
        if (b->x < -bx) { b->x = -bx; b->vx = -b->vx; bumped_arena = true; }
        if (b->z >  bz) { b->z =  bz; b->vz = -b->vz; bumped_arena = true; }
        if (b->z < -bz) { b->z = -bz; b->vz = -b->vz; bumped_arena = true; }
        if (is_llm && bumped_arena) g_tel.arena_bumps++;

        float px = b->x, pz = b->z;
        walls_push_out_bot(&b->x, &b->z, &b->vx, &b->vz);
        if (is_llm && (b->x != px || b->z != pz)) g_tel.wall_bumps++;
    }
}

void update_projectiles(Proj *projs, int *pcount, Bot *bots, int bcount, float dt) {
    for (int i = 0; i < *pcount; i++) {
        Proj *p = &projs[i];
        if (!p->active) continue;

        float old_x = p->x;
        float old_z = p->z;
        p->x += p->dir_x * p->speed * dt;
        p->z += p->dir_z * p->speed * dt;
        p->lifetime -= dt;

        bool dead = p->lifetime <= 0.0f
                 || fabsf(p->x) > g_arena_half_x + 2.0f
                 || fabsf(p->z) > g_arena_half_z + 2.0f;

        /* Check bot hits BEFORE wall blocking so projectiles reach bots near walls */
        if (!dead) {
            for (int j = 0; j < bcount; j++) {
                Bot *b = &bots[j];
                if (!b->active) continue;
                if (j == p->owner_idx) continue;
                if (b->script_id == p->owner_script) continue;
                float dx = p->x - b->x;
                float dz = p->z - b->z;
                if (dx * dx + dz * dz < HIT_RADIUS2) {
                    b->hp -= p->damage;
                    if (p->owner_script == LLM_SCRIPT_IDX) {
                        g_llm_damage += p->damage;
                        g_tel.shots_hit++;
                    }

                    Color ic = {p->r, p->g, p->b, 255};
                    fx_impact(p->x, 0.25f, p->z, ic);

                    if (b->hp <= 0.0f) {
                        if (p->owner_script == LLM_SCRIPT_IDX)
                            g_llm_kills++;
                        fx_explosion(b->x, b->z);
                        if (b->L) { lua_close(b->L); b->L = NULL; }
                        b->active = false;
                    }
                    dead = true;
                    break;
                }
            }
        }

        if (!dead)
            dead = walls_block_segment(old_x, old_z, p->x, p->z);

        if (dead)
            p->active = false;
    }
}
