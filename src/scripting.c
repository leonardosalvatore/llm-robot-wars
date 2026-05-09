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
    float weight;          /* contributes to total weight */
    float turret_turn;     /* rad/s — slowest weapon limits turret turn rate */
} WeaponStats;

static const WeaponStats WEAPON_STATS[3] = {
    [WEAPON_MACHINE_GUN] = { .damage =  5.0f, .speed = 20.0f, .lifetime = 3.0f, .fire_interval = 0.12f, .weight = 0.3f, .turret_turn = 8.0f },
    [WEAPON_AUTO_CANNON] = { .damage = 25.0f, .speed = 15.0f, .lifetime = 6.0f, .fire_interval = 0.60f, .weight = 0.9f, .turret_turn = 4.0f },
    [WEAPON_LASER]       = { .damage =  2.0f, .speed = 90.0f, .lifetime = 1.0f, .fire_interval = 0.08f, .weight = 0.4f, .turret_turn = 2.0f },
};

float scripting_weapon_fire_interval(WeaponType t) {
    if (t < 0 || t > 2) return 0.1f;
    return WEAPON_STATS[t].fire_interval;
}

/* ----------------------------------------------------------------------- */
typedef struct {
    float base_speed;   /* units/s before weight factor */
    float base_turn;    /* rad/s before weight factor */
    float lift;         /* lift capacity; higher = less affected by weight */
} LocomotionStats;

static const LocomotionStats LOCO_STATS[4] = {
    [LOCO_WHEELS] = { .base_speed = 5.0f, .base_turn = 10.0f, .lift = 2.5f },
    [LOCO_TRACKS] = { .base_speed = 2.5f, .base_turn = 12.0f, .lift = 4.5f },
    [LOCO_LEGS_4] = { .base_speed = 4.0f, .base_turn =  7.0f, .lift = 3.0f },
    [LOCO_LEGS_2] = { .base_speed = 6.0f, .base_turn =  5.0f, .lift = 1.5f },
};

typedef struct {
    float sx, sy, sz;   /* scale relative to CUBE_SIZE */
    float hp;
    float weight;
} BodyStats;

static const BodyStats BODY_STATS[7] = {
    [BODY_CUBE]     = { .sx = 1.0f, .sy = 1.0f, .sz = 1.0f, .hp = 150.0f, .weight = 1.0f },
    [BODY_TALL]     = { .sx = 0.7f, .sy = 1.6f, .sz = 0.7f, .hp = 130.0f, .weight = 0.9f },
    [BODY_FLAT]     = { .sx = 1.6f, .sy = 0.4f, .sz = 1.6f, .hp = 170.0f, .weight = 1.4f },
    [BODY_LONG_LOW] = { .sx = 0.8f, .sy = 0.4f, .sz = 1.6f, .hp = 140.0f, .weight = 1.0f },
    [BODY_TOWER]    = { .sx = 0.6f, .sy = 2.0f, .sz = 0.6f, .hp = 120.0f, .weight = 0.8f },
    [BODY_WEDGE]    = { .sx = 1.2f, .sy = 0.7f, .sz = 1.4f, .hp = 160.0f, .weight = 1.1f },
    [BODY_TANK]     = { .sx = 1.4f, .sy = 1.0f, .sz = 1.4f, .hp = 230.0f, .weight = 2.0f },
};

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

static Locomotion parse_locomotion(const char *name) {
    if (!name) return LOCO_WHEELS;
    if (strcmp(name, "wheels") == 0) return LOCO_WHEELS;
    if (strcmp(name, "tracks") == 0) return LOCO_TRACKS;
    if (strcmp(name, "4legs")  == 0) return LOCO_LEGS_4;
    if (strcmp(name, "2legs")  == 0) return LOCO_LEGS_2;
    fprintf(stderr, "[script] Unknown locomotion '%s', defaulting to wheels\n", name);
    return LOCO_WHEELS;
}

static const char *locomotion_name(Locomotion l) {
    switch (l) {
        case LOCO_WHEELS: return "wheels";
        case LOCO_TRACKS: return "tracks";
        case LOCO_LEGS_4: return "4legs";
        case LOCO_LEGS_2: return "2legs";
        default:          return "wheels";
    }
}

static BodyShape parse_body(const char *name) {
    if (!name) return BODY_CUBE;
    if (strcmp(name, "cube")     == 0) return BODY_CUBE;
    if (strcmp(name, "tall")     == 0) return BODY_TALL;
    if (strcmp(name, "flat")     == 0) return BODY_FLAT;
    if (strcmp(name, "long_low") == 0) return BODY_LONG_LOW;
    if (strcmp(name, "tower")    == 0) return BODY_TOWER;
    if (strcmp(name, "wedge")    == 0) return BODY_WEDGE;
    if (strcmp(name, "tank")     == 0) return BODY_TANK;
    fprintf(stderr, "[script] Unknown body '%s', defaulting to cube\n", name);
    return BODY_CUBE;
}

static const char *body_name(BodyShape b) {
    switch (b) {
        case BODY_CUBE:     return "cube";
        case BODY_TALL:     return "tall";
        case BODY_FLAT:     return "flat";
        case BODY_LONG_LOW: return "long_low";
        case BODY_TOWER:    return "tower";
        case BODY_WEDGE:    return "wedge";
        case BODY_TANK:     return "tank";
        default:            return "cube";
    }
}

static WeaponMount parse_mount(const char *name) {
    if (!name) return MOUNT_LEFT;
    if (strcmp(name, "left")      == 0) return MOUNT_LEFT;
    if (strcmp(name, "right")     == 0) return MOUNT_RIGHT;
    if (strcmp(name, "top")       == 0) return MOUNT_TOP;
    if (strcmp(name, "top_front") == 0) return MOUNT_TOP_FRONT;
    if (strcmp(name, "top_rear")  == 0) return MOUNT_TOP_REAR;
    fprintf(stderr, "[script] Unknown mount '%s', defaulting to left\n", name);
    return MOUNT_LEFT;
}

/* ----------------------------------------------------------------------- */
/* Compute mount offset in body-local turret coordinates.
 * Forward (turret-aim direction) is +x_local; left is +z_local.
 * Returns offset_forward (along aim axis) and offset_lateral (perpendicular). */
static void mount_offset(WeaponMount m, float body_sx, float body_sz,
                         float *off_forward, float *off_lateral)
{
    float half_x = (CUBE_SIZE * body_sx) * 0.5f + 0.05f;
    float half_z = (CUBE_SIZE * body_sz) * 0.5f + 0.05f;
    switch (m) {
        case MOUNT_LEFT:      *off_forward = 0.0f;     *off_lateral = -half_x; break;
        case MOUNT_RIGHT:     *off_forward = 0.0f;     *off_lateral = +half_x; break;
        case MOUNT_TOP:       *off_forward = 0.0f;     *off_lateral = 0.0f;    break;
        case MOUNT_TOP_FRONT: *off_forward = +half_z;  *off_lateral = 0.0f;    break;
        case MOUNT_TOP_REAR:  *off_forward = -half_z;  *off_lateral = 0.0f;    break;
        default:              *off_forward = 0.0f;     *off_lateral = 0.0f;    break;
    }
}

/* Spawn one projectile into the global g_projs[] array */
static void spawn_projectile(int owner_idx, int owner_script,
                              float ox, float oz,
                              float dir_x, float dir_z,
                              float off_forward, float off_lateral,
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

    /* Right-perpendicular of (dir_x, dir_z) in the xz plane (y up).
     * In body/turret-local coords +z is forward and +x is right; off_lateral
     * is positive on the right side. */
    float lat_x = +dir_z;
    float lat_z = -dir_x;

    Proj *p = &g_projs[slot];
    p->active       = true;
    p->x            = ox + dir_x * off_forward + lat_x * off_lateral;
    p->y            = 0.0f;
    p->z            = oz + dir_z * off_forward + lat_z * off_lateral;
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
/* Try to fire a single weapon by index. Returns 1 if a shot was emitted. */
static int try_fire_weapon(Bot *b, int bot_idx, int w, float dir_x, float dir_z) {
    if (w < 0 || w >= b->config.weapon_count) return 0;
    if (b->inertia.weapon_cd[w] > 0.0f) return 0;
    const WeaponSlot *ws = &b->config.weapons[w];
    float off_forward, off_lateral;
    mount_offset(ws->mount, b->config.body_sx, b->config.body_sz,
                 &off_forward, &off_lateral);
    spawn_projectile(bot_idx, b->script_id, b->x, b->z,
                     dir_x, dir_z, off_forward, off_lateral, ws->type);
    b->inertia.weapon_cd[w] = WEAPON_STATS[ws->type].fire_interval;
    return 1;
}

/* fire(dx, dz) — aim turret toward (dx,dz) and fire ALL mounted weapons. */
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

    int shots = 0;
    for (int w = 0; w < b->config.weapon_count; w++) {
        shots += try_fire_weapon(b, idx, w, fire_dx, fire_dz);
    }
    if (shots > 0 && b->script_id == LLM_SCRIPT_IDX) {
        update_telemetry_inc_fire_frame();
        update_telemetry_inc_shots_fired(shots);
    }
    return 0;
}

/* fire_weapon(idx, dx, dz) — aim turret toward (dx,dz) and fire ONLY weapon idx (1-based). */
static int lua_api_fire_weapon(lua_State *L) {
    int   one_based = (int)luaL_checkinteger(L, 1);
    float dx = (float)luaL_checknumber(L, 2);
    float dz = (float)luaL_checknumber(L, 3);

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

    int w = one_based - 1; /* Lua is 1-based */
    int shots = try_fire_weapon(b, idx, w, fire_dx, fire_dz);
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
    lua_register(L, "move",        lua_api_move);
    lua_register(L, "fire",        lua_api_fire);
    lua_register(L, "fire_weapon", lua_api_fire_weapon);
    lua_register(L, "scan",        lua_api_scan);
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

/* ----------------------------------------------------------------------- */
/* Apply lift/weight model to derive max_hp, max_speed, turn_rate, dims. */
static void finalize_config(BotConfig *out) {
    const BodyStats       *bs = &BODY_STATS[(int)out->body];
    const LocomotionStats *ls = &LOCO_STATS[(int)out->locomotion];

    out->body_sx = bs->sx;
    out->body_sy = bs->sy;
    out->body_sz = bs->sz;
    out->max_hp  = bs->hp;

    float w = bs->weight;
    for (int i = 0; i < out->weapon_count; i++) {
        w += WEAPON_STATS[(int)out->weapons[i].type].weight;
    }
    out->total_weight = w;

    float factor      = ls->lift / (ls->lift + w);
    out->max_speed    = ls->base_speed * factor;
    out->turn_rate    = ls->base_turn  * factor;
}

/* Synthesize a 2-weapon array from legacy left_weapon/right_weapon fields. */
static void synth_legacy_weapons(BotConfig *out, WeaponType lw, WeaponType rw) {
    out->weapon_count        = 2;
    out->weapons[0].type     = lw;
    out->weapons[0].mount    = MOUNT_LEFT;
    out->weapons[1].type     = rw;
    out->weapons[1].mount    = MOUNT_RIGHT;
}

void scripting_call_init(lua_State *L, BotConfig *out) {
    /* Default config: wheels + cube + 2x AutoCannon (left/right) */
    out->locomotion   = LOCO_WHEELS;
    out->body         = BODY_CUBE;
    synth_legacy_weapons(out, WEAPON_AUTO_CANNON, WEAPON_AUTO_CANNON);

    bool have_weapons_table = false;
    bool have_left_legacy   = false;
    bool have_right_legacy  = false;
    WeaponType legacy_left  = WEAPON_AUTO_CANNON;
    WeaponType legacy_right = WEAPON_AUTO_CANNON;

    lua_getglobal(L, "init");
    if (lua_type(L, -1) != LUA_TFUNCTION) {
        lua_pop(L, 1);
        fprintf(stderr, "[script] init() not found, using defaults\n");
        finalize_config(out);
        goto export_globals;
    }

    if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
        fprintf(stderr, "[script] init() error: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
        finalize_config(out);
        goto export_globals;
    }
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        fprintf(stderr, "[script] init() did not return a table, using defaults\n");
        finalize_config(out);
        goto export_globals;
    }

    /* locomotion */
    lua_getfield(L, -1, "locomotion");
    if (lua_isstring(L, -1)) out->locomotion = parse_locomotion(lua_tostring(L, -1));
    lua_pop(L, 1);

    /* body */
    lua_getfield(L, -1, "body");
    if (lua_isstring(L, -1)) out->body = parse_body(lua_tostring(L, -1));
    lua_pop(L, 1);

    /* weapons array */
    lua_getfield(L, -1, "weapons");
    if (lua_istable(L, -1)) {
        have_weapons_table = true;
        int n = (int)lua_rawlen(L, -1);
        if (n < 1) n = 0;
        if (n > MAX_WEAPONS) {
            fprintf(stderr, "[script] weapons[] has %d entries, capping to %d\n",
                    n, MAX_WEAPONS);
            n = MAX_WEAPONS;
        }
        out->weapon_count = 0;
        bool mount_used[5] = {false, false, false, false, false};
        for (int i = 1; i <= n; i++) {
            lua_rawgeti(L, -1, i);
            if (lua_istable(L, -1)) {
                lua_getfield(L, -1, "type");
                WeaponType t = parse_weapon(lua_tostring(L, -1));
                lua_pop(L, 1);
                lua_getfield(L, -1, "mount");
                WeaponMount m = parse_mount(lua_tostring(L, -1));
                lua_pop(L, 1);
                if (mount_used[(int)m]) {
                    fprintf(stderr,
                            "[script] mount collision at slot %d, last entry wins\n", i);
                    /* Replace prior entry with the same mount */
                    for (int k = 0; k < out->weapon_count; k++) {
                        if (out->weapons[k].mount == m) {
                            out->weapons[k].type = t;
                            break;
                        }
                    }
                } else {
                    out->weapons[out->weapon_count].type  = t;
                    out->weapons[out->weapon_count].mount = m;
                    out->weapon_count++;
                    mount_used[(int)m] = true;
                }
            } else {
                fprintf(stderr,
                        "[script] weapons[%d] is not a table, ignored\n", i);
            }
            lua_pop(L, 1);
        }
        if (out->weapon_count == 0) {
            fprintf(stderr,
                    "[script] weapons[] empty, falling back to default 2x AutoCannon\n");
            synth_legacy_weapons(out, WEAPON_AUTO_CANNON, WEAPON_AUTO_CANNON);
        }
    }
    lua_pop(L, 1);

    /* legacy left_weapon / right_weapon (used only if `weapons` absent) */
    lua_getfield(L, -1, "left_weapon");
    if (lua_isstring(L, -1)) {
        have_left_legacy = true;
        legacy_left = parse_weapon(lua_tostring(L, -1));
    }
    lua_pop(L, 1);

    lua_getfield(L, -1, "right_weapon");
    if (lua_isstring(L, -1)) {
        have_right_legacy = true;
        legacy_right = parse_weapon(lua_tostring(L, -1));
    }
    lua_pop(L, 1);

    if (!have_weapons_table && (have_left_legacy || have_right_legacy)) {
        WeaponType lw = have_left_legacy  ? legacy_left  : WEAPON_AUTO_CANNON;
        WeaponType rw = have_right_legacy ? legacy_right : WEAPON_AUTO_CANNON;
        synth_legacy_weapons(out, lw, rw);
    }

    /* Legacy `armour` field is now ignored. Warn once per file load. */
    lua_getfield(L, -1, "armour");
    if (!lua_isnil(L, -1)) {
        fprintf(stderr,
                "[script] legacy 'armour' field ignored — use locomotion/body/weapons instead\n");
    }
    lua_pop(L, 1);

    lua_pop(L, 1); /* the init() result table */
    finalize_config(out);

export_globals:
    /* Per-bot globals exposed to think() */
    lua_pushstring(L, locomotion_name(out->locomotion));
    lua_setglobal(L, "self_locomotion");
    lua_pushstring(L, body_name(out->body));
    lua_setglobal(L, "self_body");

    /* self_weapons = { "MachineGun", "Laser", ... } and self_weapon_count */
    lua_newtable(L);
    for (int i = 0; i < out->weapon_count; i++) {
        lua_pushstring(L, weapon_name(out->weapons[i].type));
        lua_rawseti(L, -2, i + 1);
    }
    lua_setglobal(L, "self_weapons");
    lua_pushinteger(L, (lua_Integer)out->weapon_count);
    lua_setglobal(L, "self_weapon_count");

    /* Backward-compat: keep self_left_weapon/self_right_weapon set to the
     * weapons mounted at MOUNT_LEFT / MOUNT_RIGHT (or the first/last if
     * the script does not use those mounts). */
    {
        const char *lname = NULL;
        const char *rname = NULL;
        for (int i = 0; i < out->weapon_count; i++) {
            if (out->weapons[i].mount == MOUNT_LEFT)  lname = weapon_name(out->weapons[i].type);
            if (out->weapons[i].mount == MOUNT_RIGHT) rname = weapon_name(out->weapons[i].type);
        }
        if (!lname && out->weapon_count > 0)
            lname = weapon_name(out->weapons[0].type);
        if (!rname && out->weapon_count > 0)
            rname = weapon_name(out->weapons[out->weapon_count - 1].type);
        if (lname) { lua_pushstring(L, lname); lua_setglobal(L, "self_left_weapon");  }
        if (rname) { lua_pushstring(L, rname); lua_setglobal(L, "self_right_weapon"); }
    }

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
