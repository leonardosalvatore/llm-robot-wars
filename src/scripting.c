#include "scripting.h"
#include "walls.h"
#include "update.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static char g_last_error[512] = {0};

/* ----------------------------------------------------------------------- */
typedef struct {
    float damage;
    float speed;
    float lifetime;
    float fire_interval;   /* minimum seconds between two shots from this weapon */
} WeaponStats;

static const WeaponStats WEAPON_STATS[3] = {
    [WEAPON_MACHINE_GUN] = { .damage = 5.0f,  .speed = 20.0f, .lifetime = 1.5f, .fire_interval = 0.12f },
    [WEAPON_AUTO_CANNON] = { .damage = 25.0f, .speed = 15.0f, .lifetime = 3.0f, .fire_interval = 0.60f },
    [WEAPON_LASER]       = { .damage = 2.0f,  .speed = 90.0f, .lifetime = 0.5f, .fire_interval = 0.08f },
};

float scripting_weapon_fire_interval(WeaponType t) {
    if (t < 0 || t > 2) return 0.1f;
    return WEAPON_STATS[t].fire_interval;
}

static const float ARMOUR_MAX_HP[4]    = { 100.0f, 150.0f, 200.0f, 250.0f };
static const float ARMOUR_MAX_SPEED[4] = {   5.0f,   3.5f,   2.0f,   0.8f };
static const float ARMOUR_BODY_SCALE[4]= {   0.7f,   1.0f,   1.3f,   1.6f };

/* ----------------------------------------------------------------------- */
static WeaponType parse_weapon(const char *name) {
    if (!name) return WEAPON_AUTO_CANNON;
    if (strcmp(name, "MachineGun")  == 0) return WEAPON_MACHINE_GUN;
    if (strcmp(name, "AutoCannon")  == 0) return WEAPON_AUTO_CANNON;
    if (strcmp(name, "Laser")       == 0) return WEAPON_LASER;
    fprintf(stderr, "[script] Unknown weapon '%s', defaulting to AutoCannon\n", name);
    return WEAPON_AUTO_CANNON;
}

static const char *weapon_name(WeaponType t) {
    switch (t) {
        case WEAPON_MACHINE_GUN: return "MachineGun";
        case WEAPON_AUTO_CANNON: return "AutoCannon";
        case WEAPON_LASER:       return "Laser";
        default:                 return "AutoCannon";
    }
}

/* Spawn one projectile into the global g_projs[] array */
static void spawn_projectile(int owner_idx, int owner_script,
                              float ox, float oz,
                              float dir_x, float dir_z,
                              float offset_x,
                              WeaponType wtype)
{
    const WeaponStats *ws = &WEAPON_STATS[wtype];

    unsigned char r = 255, g = 220, b = 50;
    if (wtype == WEAPON_MACHINE_GUN) { r = 200; g = 200; b = 200; }
    if (wtype == WEAPON_LASER)       { r = 255; g =  50; b =  50; }

    /* Find an inactive slot or append */
    int slot = -1;
    for (int i = 0; i < g_proj_count; i++) {
        if (!g_projs[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        if (g_proj_count >= MAX_PROJECTILES) return;
        slot = g_proj_count++;
    }

    Proj *p = &g_projs[slot];
    p->active       = true;
    p->x            = ox + offset_x;
    p->y            = 0.0f;
    p->z            = oz;
    p->r = r; p->g = g; p->b = b; p->a = 255;
    p->owner_idx    = owner_idx;
    p->owner_script = owner_script;
    p->weapon_type  = wtype;
    p->lifetime     = ws->lifetime;
    p->dir_x        = dir_x;
    p->dir_z        = dir_z;
    p->speed        = ws->speed;
    p->damage       = ws->damage;
}

/* ----------------------------------------------------------------------- */
static int lua_api_move(lua_State *L) {
    float dx = (float)luaL_checknumber(L, 1);
    float dz = (float)luaL_checknumber(L, 2);

    lua_getglobal(L, "__bot_idx");
    int idx = (int)lua_tointeger(L, -1);
    lua_pop(L, 1);

    if (idx < 0 || idx >= g_bot_count) return 0;
    Bot *b = &g_bots[idx];
    if (!b->active) return 0;

    float len = sqrtf(dx * dx + dz * dz);
    if (len < 1e-6f) return 0;

    b->inertia.desired_body_angle = atan2f(dz / len, dx / len);
    b->inertia.move_requested     = 1;
    return 0;
}

/* ----------------------------------------------------------------------- */
static int lua_api_fire(lua_State *L) {
    float dx = (float)luaL_checknumber(L, 1);
    float dz = (float)luaL_checknumber(L, 2);

    lua_getglobal(L, "__bot_idx");
    int idx = (int)lua_tointeger(L, -1);
    lua_pop(L, 1);

    if (idx < 0 || idx >= g_bot_count) return 0;
    Bot *b = &g_bots[idx];
    if (!b->active) return 0;

    float len = sqrtf(dx * dx + dz * dz);
    if (len < 1e-6f) return 0;

    b->inertia.desired_turret_angle = atan2f(dz / len, dx / len);

    float fire_dx = cosf(b->inertia.turret_angle);
    float fire_dz = sinf(b->inertia.turret_angle);

    float half_body = (CUBE_SIZE * b->config.body_scale) * 0.5f + 0.05f;

    int shots = 0;
    if (b->inertia.left_fire_cd <= 0.0f) {
        spawn_projectile(idx, b->script_id, b->x, b->z,
                         fire_dx, fire_dz, -half_body, b->config.left_weapon);
        b->inertia.left_fire_cd = WEAPON_STATS[b->config.left_weapon].fire_interval;
        shots++;
    }
    if (b->inertia.right_fire_cd <= 0.0f) {
        spawn_projectile(idx, b->script_id, b->x, b->z,
                         fire_dx, fire_dz, +half_body, b->config.right_weapon);
        b->inertia.right_fire_cd = WEAPON_STATS[b->config.right_weapon].fire_interval;
        shots++;
    }
    if (shots > 0 && b->script_id == LLM_SCRIPT_IDX) {
        update_telemetry_inc_fire_frame();
        update_telemetry_inc_shots_fired(shots);
    }
    return 0;
}

/* ----------------------------------------------------------------------- */
static int lua_api_scan(lua_State *L) {
    /* radius argument kept for backward-compat but ignored — scan is infinite */
    (void)luaL_checknumber(L, 1);

    lua_getglobal(L, "__bot_idx");
    int self_idx = (int)lua_tointeger(L, -1);
    lua_pop(L, 1);

    if (self_idx < 0 || self_idx >= g_bot_count) {
        lua_newtable(L);
        return 1;
    }
    Bot *self = &g_bots[self_idx];
    if (!self->active) { lua_newtable(L); return 1; }

    float sx = self->x;
    float sz = self->z;

    /* reset hit list for this frame */
    self->inertia.scan_hit_count = 0;

    bool self_is_llm = (self->script_id == LLM_SCRIPT_IDX);
    float nearest_enemy_dist = 0.0f;
    bool  saw_enemy = false;

    lua_newtable(L);
    int entry = 1;

    /* Bots — infinite range, LOS-only.
     * Non-LLM bots cannot see each other; only the LLM bot is visible to them. */
    for (int i = 0; i < g_bot_count; i++) {
        if (i == self_idx) continue;
        Bot *b = &g_bots[i];
        if (!b->active) continue;

        bool target_is_llm = (b->script_id == LLM_SCRIPT_IDX);
        if (!self_is_llm && !target_is_llm) continue;

        if (walls_block_segment(sx, sz, b->x, b->z)) continue;

        float ddx  = b->x - sx;
        float ddz  = b->z - sz;
        float dist = sqrtf(ddx * ddx + ddz * ddz);

        if (self_is_llm && b->script_id != LLM_SCRIPT_IDX) {
            if (!saw_enemy || dist < nearest_enemy_dist) {
                nearest_enemy_dist = dist;
                saw_enemy = true;
            }
        }

        int h = self->inertia.scan_hit_count;
        if (h < MAX_SCAN_HITS) {
            self->inertia.scan_hit_x[h] = b->x;
            self->inertia.scan_hit_z[h] = b->z;
            self->inertia.scan_hit_type[h] = 0;
            self->inertia.scan_hit_count++;
        }

        lua_newtable(L);
        lua_pushstring(L, "bot");                        lua_setfield(L, -2, "type");
        lua_pushnumber(L, (double)b->x);                 lua_setfield(L, -2, "x");
        lua_pushnumber(L, (double)b->z);                 lua_setfield(L, -2, "z");
        lua_pushnumber(L, (double)dist);                 lua_setfield(L, -2, "distance");
        lua_pushinteger(L, b->script_id);                lua_setfield(L, -2, "team");
        lua_pushnumber(L, (double)b->hp);                lua_setfield(L, -2, "hp");
        lua_pushnumber(L, (double)b->config.max_hp);     lua_setfield(L, -2, "max_hp");
        lua_rawseti(L, -2, entry++);
    }

    /* Walls — infinite range, no LOS check needed */
    int         wn = walls_count();
    const Wall *wv = walls_get();
    for (int i = 0; i < wn; i++) {
        const Wall *w = &wv[i];
        float clamp_x = sx < w->x - w->hw ? w->x - w->hw :
                        sx > w->x + w->hw ? w->x + w->hw : sx;
        float clamp_z = sz < w->z - w->hd ? w->z - w->hd :
                        sz > w->z + w->hd ? w->z + w->hd : sz;
        float ddx  = clamp_x - sx;
        float ddz  = clamp_z - sz;
        float dist = sqrtf(ddx * ddx + ddz * ddz);

        int h = self->inertia.scan_hit_count;
        if (h < MAX_SCAN_HITS) {
            self->inertia.scan_hit_x[h] = clamp_x;
            self->inertia.scan_hit_z[h] = clamp_z;
            self->inertia.scan_hit_type[h] = 1;
            self->inertia.scan_hit_count++;
        }

        lua_newtable(L);
        lua_pushstring(L, "wall");          lua_setfield(L, -2, "type");
        lua_pushnumber(L, (double)clamp_x); lua_setfield(L, -2, "x");
        lua_pushnumber(L, (double)clamp_z); lua_setfield(L, -2, "z");
        lua_pushnumber(L, (double)dist);    lua_setfield(L, -2, "distance");
        lua_rawseti(L, -2, entry++);
    }

    if (self_is_llm) {
        update_telemetry_inc_think_frame(saw_enemy, nearest_enemy_dist);
    }
    return 1;
}

/* ----------------------------------------------------------------------- */
static void register_api(lua_State *L) {
    lua_register(L, "move", lua_api_move);
    lua_register(L, "fire", lua_api_fire);
    lua_register(L, "scan", lua_api_scan);
}

/* ----------------------------------------------------------------------- */

void scripting_init(void) {
    /* Nothing to set up now that ECS is gone */
}

lua_State *scripting_load(const char *path) {
    g_last_error[0] = '\0';

    lua_State *L = luaL_newstate();
    if (!L) return NULL;
    luaL_openlibs(L);
    register_api(L);
    if (luaL_dofile(L, path) != LUA_OK) {
        const char *msg = lua_tostring(L, -1);
        fprintf(stderr, "[script] Error loading '%s': %s\n", path, msg);
        if (msg)
            snprintf(g_last_error, sizeof(g_last_error), "%s", msg);
        lua_close(L);
        return NULL;
    }
    return L;
}

const char *scripting_get_last_error(void) {
    return g_last_error[0] ? g_last_error : NULL;
}

bool scripting_check_syntax_file(const char *path, char *err_buf, int err_size) {
    lua_State *L = luaL_newstate();
    if (!L) return false;
    int rc = luaL_loadfile(L, path);
    if (rc != LUA_OK) {
        const char *msg = lua_tostring(L, -1);
        if (msg && err_buf)
            snprintf(err_buf, (size_t)err_size, "%s", msg);
        lua_close(L);
        return false;
    }
    lua_close(L);
    return true;
}

void scripting_call_init(lua_State *L, BotConfig *out) {
    out->left_weapon  = WEAPON_AUTO_CANNON;
    out->right_weapon = WEAPON_AUTO_CANNON;
    out->armour       = 0;

    lua_getglobal(L, "init");
    if (lua_type(L, -1) != LUA_TFUNCTION) {
        lua_pop(L, 1);
        fprintf(stderr, "[script] init() not found, using defaults\n");
    } else {
        if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
            fprintf(stderr, "[script] init() error: %s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
        } else if (lua_istable(L, -1)) {
            lua_getfield(L, -1, "left_weapon");
            out->left_weapon = parse_weapon(lua_tostring(L, -1));
            lua_pop(L, 1);

            lua_getfield(L, -1, "right_weapon");
            out->right_weapon = parse_weapon(lua_tostring(L, -1));
            lua_pop(L, 1);

            lua_getfield(L, -1, "armour");
            int a = (int)luaL_optinteger(L, -1, 0);
            out->armour = a < 0 ? 0 : (a > 3 ? 3 : a);
            lua_pop(L, 1);

            lua_pop(L, 1);
        } else {
            lua_pop(L, 1);
            fprintf(stderr, "[script] init() did not return a table, using defaults\n");
        }
    }

    out->max_hp     = ARMOUR_MAX_HP[out->armour];
    out->max_speed  = ARMOUR_MAX_SPEED[out->armour];
    out->body_scale = ARMOUR_BODY_SCALE[out->armour];

    lua_pushstring(L, weapon_name(out->left_weapon));
    lua_setglobal(L, "self_left_weapon");
    lua_pushstring(L, weapon_name(out->right_weapon));
    lua_setglobal(L, "self_right_weapon");
    lua_pushinteger(L, out->armour);
    lua_setglobal(L, "self_armour");
    lua_pushnumber(L, (double)out->max_hp);
    lua_setglobal(L, "self_max_hp");
    lua_pushnumber(L, (double)out->max_speed);
    lua_setglobal(L, "self_max_speed");
}

void scripting_shutdown(void) {
    /* Close all bot Lua states */
    for (int i = 0; i < g_bot_count; i++) {
        if (g_bots[i].L) {
            lua_close(g_bots[i].L);
            g_bots[i].L = NULL;
        }
    }
}
