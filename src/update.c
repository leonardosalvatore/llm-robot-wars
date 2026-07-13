#include "update.h"
#include "walls.h"
#include "fx.h"
#include "scripting.h"

#include <Python.h>

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

/* Per-weapon turret turn rates in rad/s.
 * Mirrors WEAPON_STATS in scripting.c; kept local to avoid an extra header. */
static const float WEAPON_TURRET_TURN[3] = {
    [WEAPON_MACHINE_GUN] = 8.0f,
    [WEAPON_AUTO_CANNON] = 4.0f,
    [WEAPON_LASER]       = 2.0f,
};

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
    PyGILState_STATE gs = PyGILState_Ensure();

    for (int i = 0; i < count; i++) {
        Bot *b = &bots[i];
        if (!b->active || !b->py_ns) continue;

        PyObject *ns = b->py_ns;

        /* Inject per-frame globals into the bot's namespace */
        PyDict_SetItemString(ns, "self_x",      PyFloat_FromDouble((double)b->x));
        PyDict_SetItemString(ns, "self_z",       PyFloat_FromDouble((double)b->z));
        PyDict_SetItemString(ns, "self_team",    PyLong_FromLong((long)b->script_id));
        PyDict_SetItemString(ns, "self_hp",      PyFloat_FromDouble((double)b->hp));
        PyDict_SetItemString(ns, "self_max_hp",  PyFloat_FromDouble((double)b->config.max_hp));

        /* Navigation + coordination context (added for smarter bots). */
        {
            PyObject *v_id = PyLong_FromLong((long)i);
            if (v_id) { PyDict_SetItemString(ns, "self_id", v_id); Py_DECREF(v_id); }
            PyObject *v_ax = PyFloat_FromDouble((double)g_arena_half_x);
            if (v_ax) { PyDict_SetItemString(ns, "arena_half_x", v_ax); Py_DECREF(v_ax); }
            PyObject *v_az = PyFloat_FromDouble((double)g_arena_half_z);
            if (v_az) { PyDict_SetItemString(ns, "arena_half_z", v_az); Py_DECREF(v_az); }

            /* Shared per-team blackboard: all bots on the same team get the
             * SAME dict object, so they can coordinate via reads/writes. The
             * dict takes its own reference; the accessor returns a borrowed
             * ref so we do not DECREF here. */
            PyObject *tm = scripting_team_mem(b->script_id);
            if (tm) PyDict_SetItemString(ns, "team_mem", tm);
        }

        PyObject *think_fn = PyDict_GetItemString(ns, "think");
        if (!think_fn || !PyCallable_Check(think_fn)) continue;

        /* Tell API callbacks which bot is active */
        scripting_set_current_bot(i);

        PyObject *ret = PyObject_CallFunction(think_fn, "d", (double)dt);
        if (!ret) {
            PyObject *exc_type = NULL, *exc_value = NULL, *exc_tb = NULL;
            PyErr_Fetch(&exc_type, &exc_value, &exc_tb);
            PyErr_NormalizeException(&exc_type, &exc_value, &exc_tb);

            const char *err = "(unknown error)";
            PyObject *err_str = NULL;
            if (exc_value) {
                err_str = PyObject_Str(exc_value);
                if (err_str) err = PyUnicode_AsUTF8(err_str);
            }

            if (i < MAX_BOTS_DEDUP) {
                if (strncmp(s_last_script_err[i], err, 255) != 0) {
                    fprintf(stderr, "[script] bot %d: %s\n", i, err);
                    strncpy(s_last_script_err[i], err, 255);
                    s_last_script_err[i][255] = '\0';
                    if (b->script_id == LLM_SCRIPT_IDX && g_last_runtime_error[0] == '\0') {
                        strncpy(g_last_runtime_error, err,
                                sizeof(g_last_runtime_error) - 1);
                    }
                }
            } else {
                fprintf(stderr, "[script] bot %d: %s\n", i, err);
            }

            Py_XDECREF(err_str);
            Py_XDECREF(exc_type);
            Py_XDECREF(exc_value);
            Py_XDECREF(exc_tb);
        } else {
            Py_DECREF(ret);
            if (i < MAX_BOTS_DEDUP && s_last_script_err[i][0] != '\0')
                s_last_script_err[i][0] = '\0';
        }
    }

    scripting_set_current_bot(-1);
    PyGILState_Release(gs);
}

void update_inertia(Bot *bots, int count, float dt) {
    for (int i = 0; i < count; i++) {
        Bot *b = &bots[i];
        if (!b->active) continue;

        b->inertia.body_angle = angle_step(b->inertia.body_angle,
                                           b->inertia.desired_body_angle,
                                           b->config.turn_rate, dt);

        /* Turret turn-rate is the slowest of all mounted weapons. */
        float trate = 0.0f;
        for (int w = 0; w < b->config.weapon_count; w++) {
            int wt = (int)b->config.weapons[w].type;
            if (wt < 0 || wt > 2) continue;
            float tr = WEAPON_TURRET_TURN[wt];
            if (trate == 0.0f || tr < trate) trate = tr;
        }
        if (trate <= 0.0f) trate = 4.0f;
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

        for (int w = 0; w < MAX_WEAPONS; w++) {
            if (b->inertia.weapon_cd[w] > 0.0f) b->inertia.weapon_cd[w] -= dt;
        }

        /* Walk/wheel animation phase advanced by speed magnitude. */
        float speed = sqrtf(b->vx * b->vx + b->vz * b->vz);
        b->inertia.move_anim_t += speed * dt;
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
                        if (b->py_ns) {
                            PyGILState_STATE bgs = PyGILState_Ensure();
                            Py_DECREF(b->py_ns);
                            b->py_ns = NULL;
                            PyGILState_Release(bgs);
                        }
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
