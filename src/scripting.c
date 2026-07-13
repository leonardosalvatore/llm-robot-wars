#include "scripting.h"
#include "walls.h"
#include "update.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Weapon / locomotion / body tables — same values as before.
 * ------------------------------------------------------------------------- */
typedef struct {
    float damage;
    float speed;
    float lifetime;
    float fire_interval;
    float weight;
    float turret_turn;
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

typedef struct {
    float base_speed;
    float base_turn;
    float lift;
} LocomotionStats;

static const LocomotionStats LOCO_STATS[4] = {
    [LOCO_WHEELS] = { .base_speed = 5.0f, .base_turn = 10.0f, .lift = 2.5f },
    [LOCO_TRACKS] = { .base_speed = 2.5f, .base_turn = 12.0f, .lift = 4.5f },
    [LOCO_LEGS_4] = { .base_speed = 4.0f, .base_turn =  7.0f, .lift = 3.0f },
    [LOCO_LEGS_2] = { .base_speed = 6.0f, .base_turn =  5.0f, .lift = 1.5f },
};

typedef struct {
    float sx, sy, sz;
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

/* -------------------------------------------------------------------------
 * String helpers
 * ------------------------------------------------------------------------- */
static WeaponType parse_weapon(const char *name) {
    if (!name) return WEAPON_AUTO_CANNON;
    if (strcmp(name, "MachineGun") == 0) return WEAPON_MACHINE_GUN;
    if (strcmp(name, "AutoCannon") == 0) return WEAPON_AUTO_CANNON;
    if (strcmp(name, "Laser")      == 0) return WEAPON_LASER;
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

/* -------------------------------------------------------------------------
 * Mount offset geometry (unchanged)
 * ------------------------------------------------------------------------- */
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

/* -------------------------------------------------------------------------
 * Projectile spawning (unchanged)
 * ------------------------------------------------------------------------- */
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

    int slot = -1;
    for (int i = 0; i < g_proj_count; i++) {
        if (!g_projs[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        if (g_proj_count >= MAX_PROJECTILES) return;
        slot = g_proj_count++;
    }
    float lat_x = +dir_z;
    float lat_z = -dir_x;

    Proj *p       = &g_projs[slot];
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

/* -------------------------------------------------------------------------
 * Current-bot index — set by scripting_set_current_bot() before think().
 * Python C callbacks read this to locate the right Bot in g_bots[].
 * ------------------------------------------------------------------------- */
static int g_current_bot_idx = -1;
static PyThreadState *g_main_thread_state = NULL;
static char g_last_error[512] = {0};

void scripting_set_current_bot(int idx) {
    g_current_bot_idx = idx;
}

/* -------------------------------------------------------------------------
 * Shared per-team blackboard (team_mem).
 * One persistent dict per script slot, created on first use. Every bot with
 * the same script_id is handed the SAME dict object so a team can coordinate.
 * Requires the GIL to be held.
 * ------------------------------------------------------------------------- */
static PyObject *g_team_mem[TOTAL_SCRIPTS] = {0};

PyObject *scripting_team_mem(int script_id) {
    if (script_id < 0 || script_id >= TOTAL_SCRIPTS) return NULL;
    if (!g_team_mem[script_id]) {
        g_team_mem[script_id] = PyDict_New();
    }
    return g_team_mem[script_id]; /* borrowed reference */
}

void scripting_reset_team_mem(void) {
    if (!Py_IsInitialized()) return;
    PyGILState_STATE gs = PyGILState_Ensure();
    for (int i = 0; i < TOTAL_SCRIPTS; i++) {
        if (g_team_mem[i]) {
            PyDict_Clear(g_team_mem[i]);
        }
    }
    PyGILState_Release(gs);
}

/* -------------------------------------------------------------------------
 * Python API callbacks
 * ------------------------------------------------------------------------- */
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

/* move(dx, dz) */
static PyObject *py_api_move(PyObject *self, PyObject *args) {
    (void)self;
    double dx, dz;
    if (!PyArg_ParseTuple(args, "dd", &dx, &dz)) return NULL;

    int idx = g_current_bot_idx;
    if (idx < 0 || idx >= g_bot_count) Py_RETURN_NONE;
    Bot *b = &g_bots[idx];
    if (!b->active) Py_RETURN_NONE;

    float len = sqrtf((float)(dx * dx + dz * dz));
    if (len < 1e-6f) Py_RETURN_NONE;

    b->inertia.desired_body_angle = atan2f((float)(dz / len), (float)(dx / len));
    b->inertia.move_requested     = 1;
    Py_RETURN_NONE;
}

/* fire(dx, dz) — fire ALL weapons */
static PyObject *py_api_fire(PyObject *self, PyObject *args) {
    (void)self;
    double dx, dz;
    if (!PyArg_ParseTuple(args, "dd", &dx, &dz)) return NULL;

    int idx = g_current_bot_idx;
    if (idx < 0 || idx >= g_bot_count) Py_RETURN_NONE;
    Bot *b = &g_bots[idx];
    if (!b->active) Py_RETURN_NONE;

    float len = sqrtf((float)(dx * dx + dz * dz));
    if (len < 1e-6f) Py_RETURN_NONE;

    b->inertia.desired_turret_angle = atan2f((float)(dz / len), (float)(dx / len));
    float fire_dx = cosf(b->inertia.turret_angle);
    float fire_dz = sinf(b->inertia.turret_angle);

    int shots = 0;
    for (int w = 0; w < b->config.weapon_count; w++)
        shots += try_fire_weapon(b, idx, w, fire_dx, fire_dz);

    if (shots > 0 && b->script_id == LLM_SCRIPT_IDX) {
        update_telemetry_inc_fire_frame();
        update_telemetry_inc_shots_fired(shots);
    }
    Py_RETURN_NONE;
}

/* fire_weapon(idx, dx, dz) — fire ONE weapon by 0-based index */
static PyObject *py_api_fire_weapon(PyObject *self, PyObject *args) {
    (void)self;
    int    w_idx;
    double dx, dz;
    if (!PyArg_ParseTuple(args, "idd", &w_idx, &dx, &dz)) return NULL;

    int idx = g_current_bot_idx;
    if (idx < 0 || idx >= g_bot_count) Py_RETURN_NONE;
    Bot *b = &g_bots[idx];
    if (!b->active) Py_RETURN_NONE;

    float len = sqrtf((float)(dx * dx + dz * dz));
    if (len < 1e-6f) Py_RETURN_NONE;

    b->inertia.desired_turret_angle = atan2f((float)(dz / len), (float)(dx / len));
    float fire_dx = cosf(b->inertia.turret_angle);
    float fire_dz = sinf(b->inertia.turret_angle);

    int shots = try_fire_weapon(b, idx, w_idx, fire_dx, fire_dz);
    if (shots > 0 && b->script_id == LLM_SCRIPT_IDX) {
        update_telemetry_inc_fire_frame();
        update_telemetry_inc_shots_fired(shots);
    }
    Py_RETURN_NONE;
}

/* scan(radius) — radius ignored; returns list of dicts */
static PyObject *py_api_scan(PyObject *self, PyObject *args) {
    (void)self;
    double radius;
    if (!PyArg_ParseTuple(args, "d", &radius)) {
        /* radius is optional — accept call with no args too */
        PyErr_Clear();
    }

    int self_idx = g_current_bot_idx;
    if (self_idx < 0 || self_idx >= g_bot_count) return PyList_New(0);
    Bot *scanner = &g_bots[self_idx];
    if (!scanner->active) return PyList_New(0);

    float sx = scanner->x;
    float sz = scanner->z;
    scanner->inertia.scan_hit_count = 0;

    bool self_is_llm = (scanner->script_id == LLM_SCRIPT_IDX);
    float nearest_enemy_dist = 0.0f;
    bool  saw_enemy = false;

    PyObject *result = PyList_New(0);

    /* Bots */
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

        int h = scanner->inertia.scan_hit_count;
        if (h < MAX_SCAN_HITS) {
            scanner->inertia.scan_hit_x[h]    = b->x;
            scanner->inertia.scan_hit_z[h]    = b->z;
            scanner->inertia.scan_hit_type[h] = 0;
            scanner->inertia.scan_hit_count++;
        }

        PyObject *entry = PyDict_New();
        PyDict_SetItemString(entry, "type",     PyUnicode_FromString("bot"));
        PyDict_SetItemString(entry, "x",        PyFloat_FromDouble((double)b->x));
        PyDict_SetItemString(entry, "z",        PyFloat_FromDouble((double)b->z));
        PyDict_SetItemString(entry, "distance", PyFloat_FromDouble((double)dist));
        PyDict_SetItemString(entry, "team",     PyLong_FromLong((long)b->script_id));
        PyDict_SetItemString(entry, "hp",       PyFloat_FromDouble((double)b->hp));
        PyDict_SetItemString(entry, "max_hp",   PyFloat_FromDouble((double)b->config.max_hp));
        PyList_Append(result, entry);
        Py_DECREF(entry);
    }

    /* Walls */
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

        int h = scanner->inertia.scan_hit_count;
        if (h < MAX_SCAN_HITS) {
            scanner->inertia.scan_hit_x[h]    = clamp_x;
            scanner->inertia.scan_hit_z[h]    = clamp_z;
            scanner->inertia.scan_hit_type[h] = 1;
            scanner->inertia.scan_hit_count++;
        }

        PyObject *entry = PyDict_New();
        PyDict_SetItemString(entry, "type",     PyUnicode_FromString("wall"));
        PyDict_SetItemString(entry, "x",        PyFloat_FromDouble((double)clamp_x));
        PyDict_SetItemString(entry, "z",        PyFloat_FromDouble((double)clamp_z));
        PyDict_SetItemString(entry, "distance", PyFloat_FromDouble((double)dist));
        PyList_Append(result, entry);
        Py_DECREF(entry);
    }

    if (self_is_llm)
        update_telemetry_inc_think_frame(saw_enemy, nearest_enemy_dist);

    return result;
}

/* -------------------------------------------------------------------------
 * API method table and namespace injection
 * ------------------------------------------------------------------------- */
static PyMethodDef g_api_methods[] = {
    {"move",        py_api_move,        METH_VARARGS, "move(dx,dz)"},
    {"fire",        py_api_fire,        METH_VARARGS, "fire(dx,dz)"},
    {"fire_weapon", py_api_fire_weapon, METH_VARARGS, "fire_weapon(idx,dx,dz)"},
    {"scan",        py_api_scan,        METH_VARARGS, "scan(radius)"},
    {NULL, NULL, 0, NULL}
};

static void inject_api(PyObject *ns) {
    for (int i = 0; g_api_methods[i].ml_name; i++) {
        PyObject *fn = PyCFunction_New(&g_api_methods[i], NULL);
        if (fn) {
            PyDict_SetItemString(ns, g_api_methods[i].ml_name, fn);
            Py_DECREF(fn);
        }
    }
}

/* -------------------------------------------------------------------------
 * scripting_init / scripting_shutdown
 * ------------------------------------------------------------------------- */
void scripting_init(void) {
    if (Py_IsInitialized()) return; /* only init once per process */
    Py_Initialize();
    /* Release GIL so that PyGILState_Ensure works correctly in any thread */
    g_main_thread_state = PyEval_SaveThread();
}

void scripting_shutdown(void) {
    /* Release bot namespaces for the current match. */
    if (!Py_IsInitialized()) return;
    /* Wipe shared team blackboards so state does not leak between matches. */
    scripting_reset_team_mem();
    for (int i = 0; i < g_bot_count; i++) {
        if (g_bots[i].py_ns) {
            PyGILState_STATE gs = PyGILState_Ensure();
            Py_DECREF(g_bots[i].py_ns);
            g_bots[i].py_ns = NULL;
            PyGILState_Release(gs);
        }
    }
    /* NOTE: Py_Finalize() is intentionally NOT called here because
     * scripting_shutdown() is invoked between matches.  The interpreter
     * is torn down once at true program exit (see scripting_finalize()). */
}

void scripting_finalize(void) {
    if (!Py_IsInitialized()) return;
    if (g_main_thread_state) {
        PyEval_RestoreThread(g_main_thread_state);
        g_main_thread_state = NULL;
    }
    Py_Finalize();
}

/* -------------------------------------------------------------------------
 * scripting_load
 * ------------------------------------------------------------------------- */
PyObject *scripting_load(const char *path) {
    g_last_error[0] = '\0';

    /* Read source file */
    FILE *f = fopen(path, "r");
    if (!f) {
        snprintf(g_last_error, sizeof(g_last_error), "cannot open '%s'", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    rewind(f);
    char *source = (char *)malloc((size_t)fsize + 1);
    if (!source) { fclose(f); return NULL; }
    fsize = (long)fread(source, 1, (size_t)fsize, f);
    source[fsize] = '\0';
    fclose(f);

    PyGILState_STATE gs = PyGILState_Ensure();

    /* Create a fresh namespace dict */
    PyObject *ns = PyDict_New();

    /* Inject __builtins__ (needed for exec / import) */
    PyObject *builtins = PyImport_ImportModule("builtins");
    if (builtins) {
        PyDict_SetItemString(ns, "__builtins__", builtins);
        Py_DECREF(builtins);
    }

    /* Inject game API functions */
    inject_api(ns);

    /* Execute the script in the namespace */
    PyObject *result = PyRun_String(source, Py_file_input, ns, ns);
    free(source);

    if (!result) {
        PyObject *exc_type = NULL, *exc_value = NULL, *exc_tb = NULL;
        PyErr_Fetch(&exc_type, &exc_value, &exc_tb);
        PyErr_NormalizeException(&exc_type, &exc_value, &exc_tb);
        if (exc_value) {
            PyObject *str = PyObject_Str(exc_value);
            if (str) {
                snprintf(g_last_error, sizeof(g_last_error), "%s",
                         PyUnicode_AsUTF8(str));
                Py_DECREF(str);
            }
        }
        Py_XDECREF(exc_type);
        Py_XDECREF(exc_value);
        Py_XDECREF(exc_tb);
        fprintf(stderr, "[script] Error loading '%s': %s\n", path, g_last_error);
        Py_DECREF(ns);
        PyGILState_Release(gs);
        return NULL;
    }
    Py_DECREF(result);

    PyGILState_Release(gs);
    return ns;
}

const char *scripting_get_last_error(void) {
    return g_last_error[0] ? g_last_error : NULL;
}

bool scripting_check_syntax_file(const char *path, char *err_buf, int err_size) {
    FILE *f = fopen(path, "r");
    if (!f) {
        if (err_buf) snprintf(err_buf, (size_t)err_size, "cannot open '%s'", path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    rewind(f);
    char *source = (char *)malloc((size_t)fsize + 1);
    if (!source) { fclose(f); return false; }
    fsize = (long)fread(source, 1, (size_t)fsize, f);
    source[fsize] = '\0';
    fclose(f);

    PyGILState_STATE gs = PyGILState_Ensure();
    PyObject *code = Py_CompileString(source, path, Py_file_input);
    free(source);

    if (!code) {
        PyObject *exc_type = NULL, *exc_value = NULL, *exc_tb = NULL;
        PyErr_Fetch(&exc_type, &exc_value, &exc_tb);
        PyErr_NormalizeException(&exc_type, &exc_value, &exc_tb);
        if (exc_value && err_buf) {
            PyObject *str = PyObject_Str(exc_value);
            if (str) {
                snprintf(err_buf, (size_t)err_size, "%s", PyUnicode_AsUTF8(str));
                Py_DECREF(str);
            }
        }
        Py_XDECREF(exc_type);
        Py_XDECREF(exc_value);
        Py_XDECREF(exc_tb);
        PyGILState_Release(gs);
        return false;
    }
    Py_DECREF(code);
    PyGILState_Release(gs);
    return true;
}

/* -------------------------------------------------------------------------
 * Config helpers
 * ------------------------------------------------------------------------- */
static void finalize_config(BotConfig *out) {
    const BodyStats       *bs = &BODY_STATS[(int)out->body];
    const LocomotionStats *ls = &LOCO_STATS[(int)out->locomotion];
    out->body_sx = bs->sx;
    out->body_sy = bs->sy;
    out->body_sz = bs->sz;
    out->max_hp  = bs->hp;
    float w = bs->weight;
    for (int i = 0; i < out->weapon_count; i++)
        w += WEAPON_STATS[(int)out->weapons[i].type].weight;
    out->total_weight = w;
    float factor   = ls->lift / (ls->lift + w);
    out->max_speed = ls->base_speed * factor;
    out->turn_rate = ls->base_turn  * factor;
}

static void synth_legacy_weapons(BotConfig *out, WeaponType lw, WeaponType rw) {
    out->weapon_count        = 2;
    out->weapons[0].type     = lw;
    out->weapons[0].mount    = MOUNT_LEFT;
    out->weapons[1].type     = rw;
    out->weapons[1].mount    = MOUNT_RIGHT;
}

/* Helper: get a string field from a Python dict, or NULL */
static const char *dict_get_str(PyObject *d, const char *key) {
    PyObject *v = PyDict_GetItemString(d, key);
    if (!v || !PyUnicode_Check(v)) return NULL;
    return PyUnicode_AsUTF8(v);
}

/* -------------------------------------------------------------------------
 * scripting_call_init
 * ------------------------------------------------------------------------- */
void scripting_call_init(PyObject *ns, BotConfig *out) {
    /* Defaults */
    out->locomotion = LOCO_WHEELS;
    out->body       = BODY_CUBE;
    synth_legacy_weapons(out, WEAPON_AUTO_CANNON, WEAPON_AUTO_CANNON);

    PyGILState_STATE gs = PyGILState_Ensure();

    PyObject *init_fn = PyDict_GetItemString(ns, "init");
    if (!init_fn || !PyCallable_Check(init_fn)) {
        fprintf(stderr, "[script] init() not found, using defaults\n");
        finalize_config(out);
        goto export_globals;
    }

    PyObject *cfg = PyObject_CallObject(init_fn, NULL);
    if (!cfg) {
        PyErr_Print();
        fprintf(stderr, "[script] init() error, using defaults\n");
        finalize_config(out);
        goto export_globals;
    }
    if (!PyDict_Check(cfg)) {
        Py_DECREF(cfg);
        fprintf(stderr, "[script] init() did not return a dict, using defaults\n");
        finalize_config(out);
        goto export_globals;
    }

    /* locomotion */
    const char *loco_str = dict_get_str(cfg, "locomotion");
    if (loco_str) out->locomotion = parse_locomotion(loco_str);

    /* body */
    const char *body_str = dict_get_str(cfg, "body");
    if (body_str) out->body = parse_body(body_str);

    /* weapons list */
    bool have_weapons_table = false;
    PyObject *weapons_list = PyDict_GetItemString(cfg, "weapons");
    if (weapons_list && PyList_Check(weapons_list)) {
        have_weapons_table = true;
        Py_ssize_t n = PyList_Size(weapons_list);
        if (n > MAX_WEAPONS) {
            fprintf(stderr, "[script] weapons[] has %d entries, capping to %d\n",
                    (int)n, MAX_WEAPONS);
            n = MAX_WEAPONS;
        }
        out->weapon_count = 0;
        bool mount_used[5] = {false, false, false, false, false};
        for (Py_ssize_t i = 0; i < n; i++) {
            PyObject *w = PyList_GetItem(weapons_list, i);
            if (!w || !PyDict_Check(w)) {
                fprintf(stderr, "[script] weapons[%d] is not a dict, ignored\n", (int)i);
                continue;
            }
            const char *type_str  = dict_get_str(w, "type");
            const char *mount_str = dict_get_str(w, "mount");
            WeaponType  wtype = parse_weapon(type_str);
            WeaponMount wmount = parse_mount(mount_str);
            if (mount_used[(int)wmount]) {
                fprintf(stderr, "[script] mount collision at slot %d, last entry wins\n", (int)i);
                for (int k = 0; k < out->weapon_count; k++) {
                    if (out->weapons[k].mount == wmount) {
                        out->weapons[k].type = wtype;
                        break;
                    }
                }
            } else {
                out->weapons[out->weapon_count].type  = wtype;
                out->weapons[out->weapon_count].mount = wmount;
                out->weapon_count++;
                mount_used[(int)wmount] = true;
            }
        }
        if (out->weapon_count == 0) {
            fprintf(stderr, "[script] weapons[] empty, falling back to default 2x AutoCannon\n");
            synth_legacy_weapons(out, WEAPON_AUTO_CANNON, WEAPON_AUTO_CANNON);
        }
    }

    /* Legacy left_weapon / right_weapon (if no weapons list) */
    if (!have_weapons_table) {
        const char *lw_str = dict_get_str(cfg, "left_weapon");
        const char *rw_str = dict_get_str(cfg, "right_weapon");
        if (lw_str || rw_str) {
            WeaponType lw = lw_str ? parse_weapon(lw_str) : WEAPON_AUTO_CANNON;
            WeaponType rw = rw_str ? parse_weapon(rw_str) : WEAPON_AUTO_CANNON;
            synth_legacy_weapons(out, lw, rw);
        }
    }

    Py_DECREF(cfg);
    finalize_config(out);

export_globals:
    /* Export per-bot globals into the namespace so think() can read them */
    PyDict_SetItemString(ns, "self_locomotion",
                         PyUnicode_FromString(locomotion_name(out->locomotion)));
    PyDict_SetItemString(ns, "self_body",
                         PyUnicode_FromString(body_name(out->body)));

    /* self_weapons = ["MachineGun", ...] */
    PyObject *wlist = PyList_New(0);
    for (int i = 0; i < out->weapon_count; i++)
        PyList_Append(wlist, PyUnicode_FromString(weapon_name(out->weapons[i].type)));
    PyDict_SetItemString(ns, "self_weapons", wlist);
    Py_DECREF(wlist);
    PyDict_SetItemString(ns, "self_weapon_count", PyLong_FromLong((long)out->weapon_count));

    /* Backward-compat: self_left_weapon / self_right_weapon */
    const char *lname = NULL, *rname = NULL;
    for (int i = 0; i < out->weapon_count; i++) {
        if (out->weapons[i].mount == MOUNT_LEFT)  lname = weapon_name(out->weapons[i].type);
        if (out->weapons[i].mount == MOUNT_RIGHT) rname = weapon_name(out->weapons[i].type);
    }
    if (!lname && out->weapon_count > 0) lname = weapon_name(out->weapons[0].type);
    if (!rname && out->weapon_count > 0) rname = weapon_name(out->weapons[out->weapon_count - 1].type);
    if (lname) PyDict_SetItemString(ns, "self_left_weapon",  PyUnicode_FromString(lname));
    if (rname) PyDict_SetItemString(ns, "self_right_weapon", PyUnicode_FromString(rname));

    PyDict_SetItemString(ns, "self_max_hp",    PyFloat_FromDouble((double)out->max_hp));
    PyDict_SetItemString(ns, "self_max_speed", PyFloat_FromDouble((double)out->max_speed));

    PyGILState_Release(gs);
}
