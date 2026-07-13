#include "llama_bot.h"
#include "llama_client.h"

#include <Python.h>

#include <pthread.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdarg.h>
#include <math.h>
#include <ctype.h>

#define SCRIPT_BUF_SIZE   (32  * 1024)
#define RESPONSE_BUF_SIZE (128 * 1024)
#define PROMPT_BUF_SIZE   (24  * 1024)

/* ----------------------------------------------------------------------- */
static char g_host[80]          = {0};
static int  g_port              = 8080;
static char g_script_path[256]  = {0};
static char g_user_prompt[1024] = {0};

static pthread_mutex_t g_mutex        = PTHREAD_MUTEX_INITIALIZER;
static bool            g_thread_busy  = false;
static bool            g_script_ready = false;

static LlmLogLine    g_vis_log[LLM_VIS_LOG_LINES];
static int            g_vis_log_head        = 0;
static int            g_vis_log_count       = 0;
static int            g_last_prompt_chars   = 0;
static int            g_last_response_chars = 0;
static int            g_last_bytes_rx       = 0;
static char           g_last_model[128]     = "";
static int            g_last_prompt_tokens  = -1;
static int            g_last_completion_tokens = -1;
static int            g_last_total_tokens   = -1;
static char           g_script_status[72]   = "-";
static LlmLogColor    g_script_color        = LLOG_DIM;
static char           g_gen_error[512]      = "";
static bool           g_gen_error_pending   = false;

/* Rolling match history for richer LLM context */
#define MATCH_HISTORY_SIZE 3
static MatchStats g_match_history[MATCH_HISTORY_SIZE];
static int        g_match_history_count = 0;

/* Single-slot queue for matches arriving while generation is busy */
static bool       g_pending_match_valid = false;
static MatchStats g_pending_match;

/* Stagnation tracking — how many consecutive generations produced no change */
static int  g_stagnation_count = 0;

/* ----------------------------------------------------------------------- */
static int read_file(const char *path, char *buf, int buf_size) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int n = (int)fread(buf, 1, (size_t)(buf_size - 1), f);
    fclose(f);
    if (n < 0) n = 0;
    buf[n] = '\0';
    return n;
}

static int write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fputs(content, f);
    fclose(f);
    return 0;
}

/* FNV-1a 32-bit hash of a string — good enough for stagnation detection */
static unsigned int fnv1a(const char *s) {
    unsigned int h = 2166136261u;
    while (*s) { h ^= (unsigned char)*s++; h *= 16777619u; }
    return h;
}

/* ----------------------------------------------------------------------- */
void llm_bot_log(LlmLogColor color, const char *fmt, ...) {
    char buf[LLM_VIS_LOG_COLS];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    printf("[llm] %s\n", buf);
    fflush(stdout);

    pthread_mutex_lock(&g_mutex);
    LlmLogLine *slot = &g_vis_log[g_vis_log_head];
    strncpy(slot->text, buf, LLM_VIS_LOG_COLS - 1);
    slot->text[LLM_VIS_LOG_COLS - 1] = '\0';
    slot->color = color;
    g_vis_log_head = (g_vis_log_head + 1) % LLM_VIS_LOG_LINES;
    if (g_vis_log_count < LLM_VIS_LOG_LINES) g_vis_log_count++;
    pthread_mutex_unlock(&g_mutex);
}

void llm_bot_get_vis_state(LlmVisState *out) {
    pthread_mutex_lock(&g_mutex);
    out->is_busy          = g_thread_busy;
    out->prompt_chars     = g_last_prompt_chars;
    out->response_chars   = g_last_response_chars;
    out->bytes_rx         = g_last_bytes_rx;
    snprintf(out->server, sizeof(out->server), "%s:%d", g_host, g_port);
    strncpy(out->model, g_last_model, sizeof(out->model) - 1);
    out->model[sizeof(out->model) - 1] = '\0';
    out->prompt_tokens     = g_last_prompt_tokens;
    out->completion_tokens = g_last_completion_tokens;
    out->total_tokens      = g_last_total_tokens;
    strncpy(out->script_status, g_script_status, sizeof(out->script_status) - 1);
    out->script_color = g_script_color;

    int n = g_vis_log_count;
    out->log_count = n;
    for (int i = 0; i < n; i++) {
        int src = (g_vis_log_head - n + i + LLM_VIS_LOG_LINES) % LLM_VIS_LOG_LINES;
        out->log[i] = g_vis_log[src];
    }
    pthread_mutex_unlock(&g_mutex);
}

/* ----------------------------------------------------------------------- */
/* Extract the Python script from a model response.
 * Looks for ```python fence first, then a bare ``` fence, then falls back to
 * treating the whole response as raw source. */
static void extract_python(const char *response, char *out, int out_size) {
    const char *start = strstr(response, "```python");
    if (start) {
        start += 9;
        if (*start == '\n') start++;
    } else {
        start = strstr(response, "```");
        if (start) {
            start += 3;
            while (*start && *start != '\n') start++;
            if (*start == '\n') start++;
        }
    }

    if (start) {
        const char *end = strstr(start, "```");
        if (end) {
            int len = (int)(end - start);
            if (len >= out_size) len = out_size - 1;
            while (len > 0 && (start[len-1] == '\n' || start[len-1] == '\r'
                                || start[len-1] == ' '))
                len--;
            memcpy(out, start, (size_t)len);
            out[len] = '\0';
            return;
        }
        /* Opening fence but no closing fence — use everything after. */
        int len = (int)strlen(start);
        if (len >= out_size) len = out_size - 1;
        while (len > 0 && (start[len-1] == '\n' || start[len-1] == '\r'
                            || start[len-1] == ' '))
            len--;
        memcpy(out, start, (size_t)len);
        out[len] = '\0';
        return;
    }

    /* No fences — strip any leading backticks / language hint */
    const char *p = response;
    while (*p == '`') p++;
    while (*p == '\n' || *p == '\r') p++;
    if (strncmp(p, "python\n",   7) == 0) p += 7;
    else if (strncmp(p, "python\r\n", 8) == 0) p += 8;
    int len = (int)strlen(p);
    if (len >= out_size) len = out_size - 1;
    memcpy(out, p, (size_t)len);
    out[len] = '\0';
}

/* ----------------------------------------------------------------------- */
/* Smoke-test harness — creates an isolated Python namespace, injects stub
 * move/fire/fire_weapon/scan, runs init() + 60 frames of think() with 4
 * orbiting enemies, and verifies at least one fire call aimed within 30°.
 * Operates entirely on the source STRING — no file I/O — so callers can
 * validate before writing to disk. */

#define SMOKE_NUM_ENEMIES 4
#define SMOKE_AIM_COS     0.866   /* cos(30 deg) */

typedef struct {
    int    fire_calls_total;
    int    fire_at_enemy[SMOKE_NUM_ENEMIES];
    int    enemies_targeted_mask;
    double bot_x, bot_z;
    double enemy_x[SMOKE_NUM_ENEMIES];
    double enemy_z[SMOKE_NUM_ENEMIES];
    double enemy_vx[SMOKE_NUM_ENEMIES];
    double enemy_vz[SMOKE_NUM_ENEMIES];
} SmokeState;

static SmokeState g_smoke;

static void smoke_record_fire(double dx, double dz) {
    g_smoke.fire_calls_total++;
    double len = sqrt(dx * dx + dz * dz);
    if (len < 1e-6) return;
    double nx = dx / len, nz = dz / len;
    for (int i = 0; i < SMOKE_NUM_ENEMIES; i++) {
        double edx = g_smoke.enemy_x[i] - g_smoke.bot_x;
        double edz = g_smoke.enemy_z[i] - g_smoke.bot_z;
        double el  = sqrt(edx * edx + edz * edz);
        if (el < 1e-6) continue;
        double cosang = (nx * edx + nz * edz) / el;
        if (cosang >= SMOKE_AIM_COS) {
            g_smoke.fire_at_enemy[i]++;
            g_smoke.enemies_targeted_mask |= (1 << i);
        }
    }
}

static PyObject *py_smoke_move(PyObject *s, PyObject *args) {
    (void)s; (void)args; Py_RETURN_NONE;
}

static PyObject *py_smoke_fire(PyObject *s, PyObject *args) {
    (void)s;
    double dx, dz;
    if (!PyArg_ParseTuple(args, "dd", &dx, &dz)) return NULL;
    smoke_record_fire(dx, dz);
    Py_RETURN_NONE;
}

static PyObject *py_smoke_fire_weapon(PyObject *s, PyObject *args) {
    (void)s;
    int    w;
    double dx, dz;
    if (!PyArg_ParseTuple(args, "idd", &w, &dx, &dz)) return NULL;
    smoke_record_fire(dx, dz);
    Py_RETURN_NONE;
}

static PyObject *py_smoke_scan(PyObject *s, PyObject *args) {
    (void)s; (void)args;
    PyErr_Clear();

    PyObject *result = PyList_New(0);
    for (int i = 0; i < SMOKE_NUM_ENEMIES; i++) {
        double dx   = g_smoke.enemy_x[i] - g_smoke.bot_x;
        double dz   = g_smoke.enemy_z[i] - g_smoke.bot_z;
        double dist = sqrt(dx * dx + dz * dz);

        PyObject *entry = PyDict_New();
        PyDict_SetItemString(entry, "type",     PyUnicode_FromString("bot"));
        PyDict_SetItemString(entry, "x",        PyFloat_FromDouble(g_smoke.enemy_x[i]));
        PyDict_SetItemString(entry, "z",        PyFloat_FromDouble(g_smoke.enemy_z[i]));
        PyDict_SetItemString(entry, "distance", PyFloat_FromDouble(dist));
        PyDict_SetItemString(entry, "team",     PyLong_FromLong(1));
        PyDict_SetItemString(entry, "hp",       PyFloat_FromDouble(100.0));
        PyDict_SetItemString(entry, "max_hp",   PyFloat_FromDouble(150.0));
        PyList_Append(result, entry);
        Py_DECREF(entry);
    }

    /* Distant wall so wall-avoidance code paths are exercised */
    PyObject *wall = PyDict_New();
    PyDict_SetItemString(wall, "type",     PyUnicode_FromString("wall"));
    PyDict_SetItemString(wall, "x",        PyFloat_FromDouble(18.0));
    PyDict_SetItemString(wall, "z",        PyFloat_FromDouble(0.0));
    PyDict_SetItemString(wall, "distance", PyFloat_FromDouble(18.0));
    PyList_Append(result, wall);
    Py_DECREF(wall);

    return result;
}

static PyMethodDef g_smoke_methods[] = {
    {"move",        py_smoke_move,        METH_VARARGS, NULL},
    {"fire",        py_smoke_fire,        METH_VARARGS, NULL},
    {"fire_weapon", py_smoke_fire_weapon, METH_VARARGS, NULL},
    {"scan",        py_smoke_scan,        METH_VARARGS, NULL},
    {NULL, NULL, 0, NULL}
};

static void smoke_set_globals(PyObject *ns, double x, double z,
                               double hp, double max_hp) {
    PyDict_SetItemString(ns, "self_x",      PyFloat_FromDouble(x));
    PyDict_SetItemString(ns, "self_z",      PyFloat_FromDouble(z));
    PyDict_SetItemString(ns, "self_team",   PyLong_FromLong(6));
    PyDict_SetItemString(ns, "self_hp",     PyFloat_FromDouble(hp));
    PyDict_SetItemString(ns, "self_max_hp", PyFloat_FromDouble(max_hp));
    /* Mirror the runtime navigation/coordination globals so generated scripts
     * that read them do not NameError during validation. */
    PyDict_SetItemString(ns, "self_id",      PyLong_FromLong(0));
    PyDict_SetItemString(ns, "arena_half_x", PyFloat_FromDouble(30.0));
    PyDict_SetItemString(ns, "arena_half_z", PyFloat_FromDouble(25.0));
}

static void smoke_init_arena(void) {
    memset(&g_smoke, 0, sizeof(g_smoke));
    g_smoke.enemy_x[0]  =  3.5; g_smoke.enemy_z[0]  =  0.0;
    g_smoke.enemy_vx[0] =  0.0; g_smoke.enemy_vz[0] =  1.5;
    g_smoke.enemy_x[1]  = -3.5; g_smoke.enemy_z[1]  =  0.0;
    g_smoke.enemy_vx[1] =  0.0; g_smoke.enemy_vz[1] = -2.5;
    g_smoke.enemy_x[2]  =  0.0; g_smoke.enemy_z[2]  =  3.5;
    g_smoke.enemy_vx[2] =  3.0; g_smoke.enemy_vz[2] =  0.0;
    g_smoke.enemy_x[3]  =  0.0; g_smoke.enemy_z[3]  = -3.5;
    g_smoke.enemy_vx[3] = -1.0; g_smoke.enemy_vz[3] =  0.0;
}

static void smoke_step_enemies(double dt) {
    for (int i = 0; i < SMOKE_NUM_ENEMIES; i++) {
        g_smoke.enemy_x[i] += g_smoke.enemy_vx[i] * dt;
        g_smoke.enemy_z[i] += g_smoke.enemy_vz[i] * dt;
        double r = sqrt(g_smoke.enemy_x[i] * g_smoke.enemy_x[i]
                      + g_smoke.enemy_z[i] * g_smoke.enemy_z[i]);
        if (r > 5.0) {
            g_smoke.enemy_vx[i] = -g_smoke.enemy_vx[i];
            g_smoke.enemy_vz[i] = -g_smoke.enemy_vz[i];
        }
    }
}

/* Helper: format a Python exception into a C string */
static void fetch_py_error(char *buf, int size) {
    PyObject *exc_type = NULL, *exc_value = NULL, *exc_tb = NULL;
    PyErr_Fetch(&exc_type, &exc_value, &exc_tb);
    PyErr_NormalizeException(&exc_type, &exc_value, &exc_tb);
    buf[0] = '\0';
    if (exc_value) {
        PyObject *str = PyObject_Str(exc_value);
        if (str) {
            snprintf(buf, (size_t)size, "%s", PyUnicode_AsUTF8(str));
            Py_DECREF(str);
        }
    }
    Py_XDECREF(exc_type);
    Py_XDECREF(exc_value);
    Py_XDECREF(exc_tb);
}

/* Run the full smoke test on an in-memory Python source string.
 * This is the core validation; no file I/O happens here. */
static bool smoke_test_source(const char *source, char *err, int err_size) {
    PyGILState_STATE gs = PyGILState_Ensure();

    /* Build fresh namespace */
    PyObject *ns = PyDict_New();
    PyObject *builtins = PyImport_ImportModule("builtins");
    if (builtins) {
        PyDict_SetItemString(ns, "__builtins__", builtins);
        Py_DECREF(builtins);
    }
    for (int i = 0; g_smoke_methods[i].ml_name; i++) {
        PyObject *fn = PyCFunction_New(&g_smoke_methods[i], NULL);
        if (fn) { PyDict_SetItemString(ns, g_smoke_methods[i].ml_name, fn); Py_DECREF(fn); }
    }

    /* Stage 1: execute script with NO self_* globals set.
     * Rejects scripts whose file-scope code reads self_x / self_z / etc. */
    PyObject *exec_result = PyRun_String(source, Py_file_input, ns, ns);
    if (!exec_result) {
        char py_err[480];
        fetch_py_error(py_err, (int)sizeof(py_err));
        snprintf(err, (size_t)err_size,
                 "smoke test: file-scope load crashed: %s. "
                 "Move any code that reads self_x/self_z/self_team/self_hp "
                 "into init() or think(); those globals are None at file scope.",
                 py_err);
        Py_DECREF(ns);
        PyGILState_Release(gs);
        return false;
    }
    Py_DECREF(exec_result);

    /* Inject the shared blackboard AFTER file-scope exec (mirroring runtime,
     * where team_mem is only present during think()), so scripts that read it
     * at module scope are still rejected. One persistent object for the run so
     * cross-frame coordination patterns behave as they would in a match. */
    {
        PyObject *tm = PyDict_New();
        if (tm) { PyDict_SetItemString(ns, "team_mem", tm); Py_DECREF(tm); }
    }

    /* Stage 2: init() must exist and return a dict */
    smoke_set_globals(ns, 0.0, 0.0, 250.0, 250.0);
    PyObject *init_fn = PyDict_GetItemString(ns, "init");
    if (!init_fn || !PyCallable_Check(init_fn)) {
        snprintf(err, (size_t)err_size, "smoke test: init() missing");
        Py_DECREF(ns);
        PyGILState_Release(gs);
        return false;
    }
    PyObject *cfg = PyObject_CallObject(init_fn, NULL);
    if (!cfg) {
        char py_err[480];
        fetch_py_error(py_err, (int)sizeof(py_err));
        snprintf(err, (size_t)err_size, "smoke test init() error: %s", py_err);
        Py_DECREF(ns);
        PyGILState_Release(gs);
        return false;
    }
    if (!PyDict_Check(cfg)) {
        snprintf(err, (size_t)err_size, "smoke test: init() must return a dict");
        Py_DECREF(cfg);
        Py_DECREF(ns);
        PyGILState_Release(gs);
        return false;
    }
    Py_DECREF(cfg);

    PyObject *think_fn = PyDict_GetItemString(ns, "think");
    if (!think_fn || !PyCallable_Check(think_fn)) {
        snprintf(err, (size_t)err_size, "smoke test: think() missing");
        Py_DECREF(ns);
        PyGILState_Release(gs);
        return false;
    }

    /* Stage 3: 60-frame combat simulation — hp starts full, drops to 10% halfway */
    smoke_init_arena();
    const int    FRAMES  = 60;
    const double DT      = 0.05;
    const double FULL_HP = 250.0;

    for (int frame = 0; frame < FRAMES; frame++) {
        smoke_step_enemies(DT);
        double hp = (frame < FRAMES / 2) ? (FULL_HP * 0.95) : (FULL_HP * 0.10);
        smoke_set_globals(ns, g_smoke.bot_x, g_smoke.bot_z, hp, FULL_HP);

        think_fn = PyDict_GetItemString(ns, "think");
        if (!think_fn || !PyCallable_Check(think_fn)) {
            snprintf(err, (size_t)err_size,
                     "smoke test: think() vanished at frame %d/%d", frame + 1, FRAMES);
            Py_DECREF(ns);
            PyGILState_Release(gs);
            return false;
        }
        PyObject *ret = PyObject_CallFunction(think_fn, "d", DT);
        if (!ret) {
            char py_err[480];
            fetch_py_error(py_err, (int)sizeof(py_err));
            snprintf(err, (size_t)err_size,
                     "smoke test think() crashed at frame %d/%d: %s",
                     frame + 1, FRAMES, py_err);
            Py_DECREF(ns);
            PyGILState_Release(gs);
            return false;
        }
        Py_DECREF(ret);
    }

    Py_DECREF(ns);
    PyGILState_Release(gs);

    /* Stage 4: combat report */
    if (g_smoke.enemies_targeted_mask == 0) {
        snprintf(err, (size_t)err_size,
                 "smoke test combat report: in 3.0s of simulation with 4 enemies "
                 "orbiting at distance ~3.5u (front/back/left/right, varied "
                 "speeds) the script issued %d fire/fire_weapon call(s) but ZERO "
                 "of them were aimed within 30 degrees of ANY enemy. The bot "
                 "will not deal damage. In think(), iterate scan(0) entries, "
                 "find any with t[\"team\"] != self_team, and call "
                 "fire(t[\"x\"] - self_x, t[\"z\"] - self_z) every frame the enemy is "
                 "within firing range.",
                 g_smoke.fire_calls_total);
        return false;
    }

    int hit_count = 0;
    for (int i = 0; i < SMOKE_NUM_ENEMIES; i++)
        if (g_smoke.enemies_targeted_mask & (1 << i)) hit_count++;
    llm_bot_log(LLOG_OK,
                ">> smoke combat: aimed at %d/%d enemies, %d fire calls",
                hit_count, SMOKE_NUM_ENEMIES, g_smoke.fire_calls_total);
    err[0] = '\0';
    return true;
}

/* ----------------------------------------------------------------------- */
static void append_text(char *dst, int dst_size, int *len_io, const char *fmt, ...) {
    int len = *len_io;
    if (len >= dst_size - 1) return;
    va_list ap;
    va_start(ap, fmt);
    int wrote = vsnprintf(dst + len, (size_t)(dst_size - len), fmt, ap);
    va_end(ap);
    if (wrote < 0) return;
    len += wrote;
    if (len > dst_size - 1) len = dst_size - 1;
    *len_io = len;
}

/* ----------------------------------------------------------------------- */
/* Copy `src` into `dst`, blanking out Python comments (everything from an
 * unquoted '#' to end-of-line). Minimal quote tracking keeps '#' that appears
 * inside string literals. This stops the static checks below from tripping on
 * example/pitfall text in the COOKBOOK comment block (e.g. a comment that
 * literally reads `BAD: scan = scan(0)`). */
static void strip_py_comments(const char *src, char *dst, int dst_size) {
    int di = 0;
    char quote = 0;
    for (const char *p = src; *p && di < dst_size - 1; p++) {
        char c = *p;
        if (quote) {
            if (c == '\\' && p[1]) {
                dst[di++] = c;
                if (di < dst_size - 1) dst[di++] = p[1];
                p++;
                continue;
            }
            dst[di++] = c;
            if (c == quote) quote = 0;
        } else if (c == '#') {
            while (p[1] && p[1] != '\n') p++;   /* drop the comment body */
        } else {
            if (c == '\'' || c == '"') quote = c;
            dst[di++] = c;
        }
    }
    dst[di] = '\0';
}

/* ----------------------------------------------------------------------- */
/* Return true if `script` contains a top-level assignment to the EXACT
 * identifier `name` (i.e. `name = ...`), as opposed to a variable whose name
 * merely ends with those characters (e.g. `last_scan = scan(0)` is fine).
 * We require a non-identifier char (or start of buffer) immediately before
 * the name and a single `=` (not `==`) after optional spaces. */
static bool assigns_to_name(const char *script, const char *name) {
    size_t nlen = strlen(name);
    const char *p = script;
    while ((p = strstr(p, name)) != NULL) {
        bool left_ok = (p == script) ||
                       !(isalnum((unsigned char)p[-1]) || p[-1] == '_' || p[-1] == '.');
        const char *q = p + nlen;
        while (*q == ' ' || *q == '\t') q++;
        bool right_ok = (*q == '=' && q[1] != '=');
        if (left_ok && right_ok) return true;
        p += nlen;
    }
    return false;
}

/* ----------------------------------------------------------------------- */
/* Detect patterns in a GENERATED Python script that break at runtime. */
static void detect_generated_bugs(const char *script_raw, char *hint, int hint_size) {
    hint[0] = '\0';
    if (!script_raw) return;

    /* Work on a comment-stripped copy so the COOKBOOK pitfall examples
     * (which intentionally contain BAD patterns) do not trip the checks. */
    static char script[SCRIPT_BUF_SIZE];   /* generation runs single-threaded */
    strip_py_comments(script_raw, script, SCRIPT_BUF_SIZE);

    /* Lua keyword leakage: model emitted Lua instead of Python */
    if (strstr(script, "\nlocal ") || strncmp(script, "local ", 6) == 0) {
        snprintf(hint, (size_t)hint_size,
            "Your script contains Lua 'local' variable declarations. "
            "This is Python, not Lua. Remove all 'local' keywords; "
            "Python variables are declared by plain assignment: x = 0");
        return;
    }
    /* Lua 'then' keyword */
    if (strstr(script, " then\n") || strstr(script, " then\r")) {
        snprintf(hint, (size_t)hint_size,
            "Your script contains Lua 'then' keywords (if/elseif ... then). "
            "This is Python. Use 'if condition:' without 'then'.");
        return;
    }
    /* self.x instead of self_x */
    if (strstr(script, "self.x") || strstr(script, "self.z") ||
        strstr(script, "self.hp") || strstr(script, "self.team")) {
        snprintf(hint, (size_t)hint_size,
            "Your script uses 'self.x' / 'self.z' etc. "
            "These are injected as plain globals: use self_x, self_z, self_hp, self_team.");
        return;
    }
    /* API name shadowing: assignment to the EXACT name of an API function.
     * Word-boundary aware so legit names like `last_scan = scan(0)` are NOT
     * flagged. */
    {
        const char *api[] = {"scan", "move", "fire_weapon", "fire", NULL};
        for (int i = 0; api[i]; i++) {
            if (assigns_to_name(script, api[i])) {
                snprintf(hint, (size_t)hint_size,
                    "Your script assigns to the name '%s' which shadows the "
                    "game API function. Use a different variable name, "
                    "e.g. 'targets = scan(0)' not 'scan = scan(0)'.",
                    api[i]);
                return;
            }
        }
    }
}

static void build_known_bugs(const char *current_raw, char *known_bugs, int size) {
    int kb_len = 0;
    known_bugs[0] = '\0';

    /* Comment-stripped copy so cookbook examples don't register as bugs. */
    static char current[SCRIPT_BUF_SIZE];   /* generation runs single-threaded */
    strip_py_comments(current_raw, current, SCRIPT_BUF_SIZE);

    if (strstr(current, "\nlocal ") || strncmp(current, "local ", 6) == 0) {
        kb_len += snprintf(known_bugs + kb_len, (size_t)(size - kb_len),
            "=== CRITICAL BUG: Lua 'local' keyword in Python script ===\n"
            "Remove all 'local' keywords. Python has no 'local' keyword.\n"
            "Declare variables with plain assignment: x = 0\n\n");
    }

    if (assigns_to_name(current, "scan")) {
        kb_len += snprintf(known_bugs + kb_len, (size_t)(size - kb_len),
            "=== CRITICAL BUG: variable shadows scan() API ===\n"
            "The script assigns to 'scan' which shadows the scan() API function.\n"
            "Fix: use 'targets = scan(0)' — never name a variable after an API function.\n\n");
    }
}

static void build_system_prompt(char *dst, int dst_size) {
    snprintf(dst, (size_t)dst_size,
        "You are iteratively improving a Python script for an arena combat robot game.\n"
        "\n"
        "=== Game API (Python) ===\n"
        "move(dx, dz)           -- set movement direction; internally normalised; magnitude ignored.\n"
        "                       -- Do NOT multiply dx/dz by a speed scalar — pass a unit direction.\n"
        "fire(dx, dz)           -- aim turret toward (dx,dz) and fire ALL mounted weapons this frame.\n"
        "                       -- Each weapon has its own cooldown; safe to call every tick.\n"
        "fire_weapon(i, dx, dz) -- like fire() but only weapon i (0-based index into weapons list).\n"
        "scan(radius)           -- radius argument is ignored; returns all bots (LOS) + walls.\n"
        "                       -- entries: {\"type\":\"bot\",  \"x\":…, \"z\":…, \"distance\":…, \"team\":…, \"hp\":…, \"max_hp\":…}\n"
        "                                   {\"type\":\"wall\", \"x\":…, \"z\":…, \"distance\":…}   (x,z = nearest point)\n"
        "Per-frame globals (injected before every think() call):\n"
        "  self_x, self_z        -- world position\n"
        "  self_team             -- integer script id of this bot's team\n"
        "  self_hp, self_max_hp  -- current and maximum hit points\n"
        "  self_locomotion       -- string: \"wheels\"|\"tracks\"|\"4legs\"|\"2legs\"\n"
        "  self_body             -- string: \"cube\"|\"tall\"|\"flat\"|\"long_low\"|\"tower\"|\"wedge\"|\"tank\"\n"
        "  self_weapons          -- list of weapon-type strings, 0-based length 1..4\n"
        "  self_weapon_count     -- integer 1..4\n"
        "  self_max_speed        -- current max linear speed in units/second (after weight)\n"
        "  self_id               -- unique integer id of THIS bot, stable for the match.\n"
        "                       -- Use it to split roles across the team, e.g. self_id %% 3.\n"
        "  arena_half_x          -- arena spans x in [-arena_half_x, +arena_half_x]; centre is 0.\n"
        "  arena_half_z          -- arena spans z in [-arena_half_z, +arena_half_z]; centre is (0,0).\n"
        "  team_mem              -- a shared dict, the SAME object for every bot on your team.\n"
        "                       -- Read/write it to coordinate (focus target, rally point, roles).\n"
        "                       -- It PERSISTS across frames within a match. To stay independent\n"
        "                       -- of bot update order, prefer reading values written LAST frame.\n"
        "                       -- Always use team_mem.get(key, default); never assume a key exists.\n"
        "\n"
        "=== Navigation (use arena bounds + scan to move well) ===\n"
        "The arena is centred on (0,0) and bounded by arena_half_x / arena_half_z. Ramming the\n"
        "border wastes time (tracked as arena_bumps) and hugging walls hides you from enemies.\n"
        "- If |self_x| or |self_z| is close to its arena_half_*, add a pull toward centre:\n"
        "    cx = -self_x / max(arena_half_x, 1.0); cz = -self_z / max(arena_half_z, 1.0)\n"
        "  and blend it into your move() vector, stronger the closer you are to the edge.\n"
        "- Keep the wall-avoidance push from scan() walls; combine it with your target vector.\n"
        "- Unstick: if you keep requesting move() but your position barely changes (compare a\n"
        "  cached self_x/self_z from a few frames ago), you are jammed; pick a fresh heading.\n"
        "\n"
        "=== Team coordination (use team_mem + self_id + scan) ===\n"
        "scan() returns teammates too (entries where team == self_team). Coordinate instead of\n"
        "each bot fighting alone:\n"
        "- FOCUS FIRE: elect one shared enemy in team_mem (e.g. team_mem[\"focus_x\"]/[\"focus_z\"])\n"
        "  and have everyone shoot it, so targets die faster. Refresh the pick periodically.\n"
        "- ROLE SPLIT: use self_id %% N to assign roles: some bots push in (attack), others hang\n"
        "  back or flank (defence). Read/write role intent through team_mem when useful.\n"
        "- SPACING: steer away from nearby teammates so the team does not clump into one target.\n"
        "- REGROUP: when outnumbered or low HP, fall back toward a shared rally point (e.g. the\n"
        "  team centroid, or centre (0,0)) stored in team_mem so the team defends together.\n"
        "\n"
        "init() must return a dict with these fields:\n"
        "  \"locomotion\": \"wheels\" | \"tracks\" | \"4legs\" | \"2legs\"\n"
        "  \"body\":       \"cube\" | \"tall\" | \"flat\" | \"long_low\" | \"tower\" | \"wedge\" | \"tank\"\n"
        "  \"weapons\":    [ { \"type\": <W>, \"mount\": <M> }, ... ]    -- 1 to 4 entries\n"
        "    where <W> is \"MachineGun\" | \"AutoCannon\" | \"Laser\"\n"
        "    and   <M> is \"left\" | \"right\" | \"top\" | \"top_front\" | \"top_rear\" (each unique)\n"
        "Any field may be omitted; defaults are wheels + cube + 2x AutoCannon (left/right).\n"
        "\n"
        "=== Weapon stats (per projectile) ===\n"
        "                 damage  speed(u/s)  lifetime(s)  fire_interval(s)  weight  turret_turn(rad/s)\n"
        "  MachineGun       5      20            3.0         0.12              0.3     8\n"
        "  AutoCannon      25      15            6.0         0.60              0.9     4\n"
        "  Laser            2      90            1.0         0.08              0.4     2\n"
        "Hit radius on target is ~0.6 units. No projectile drop.\n"
        "fire() is engine-rate-limited per weapon: if its cooldown has not elapsed the\n"
        "shot is silently dropped. You can safely call fire() every frame; excess calls\n"
        "cost nothing. But to aim better, still gate fire() on having a target in range.\n"
        "\n"
        "=== Locomotion stats ===\n"
        "             base_speed  base_turn(rad/s)  lift\n"
        "  wheels       5.0          10               2.5  (balanced)\n"
        "  tracks       2.5          12               4.5  (slow but huge lift; pivots fast)\n"
        "  4legs        4.0           7               3.0  (steady)\n"
        "  2legs        6.0           5               1.5  (fast but fragile, low lift)\n"
        "\n"
        "=== Body stats ===\n"
        "            max_hp  weight  shape (sx,sy,sz)\n"
        "  cube        150    1.0    (1.0, 1.0, 1.0)\n"
        "  tall        130    0.9    (0.7, 1.6, 0.7)\n"
        "  flat        170    1.4    (1.6, 0.4, 1.6)\n"
        "  long_low    140    1.0    (0.8, 0.4, 1.6)\n"
        "  tower       120    0.8    (0.6, 2.0, 0.6)\n"
        "  wedge       160    1.1    (1.2, 0.7, 1.4)\n"
        "  tank        230    2.0    (1.4, 1.0, 1.4)\n"
        "\n"
        "=== Weight model ===\n"
        "total_w = body.weight + sum(weapon.weight)\n"
        "factor  = locomotion.lift / (locomotion.lift + total_w)\n"
        "max_speed = locomotion.base_speed * factor\n"
        "turn_rate = locomotion.base_turn  * factor\n"
        "More/heavier weapons make the bot slower in BOTH translation and rotation.\n"
        "\n"
        "=== Arena & physics ===\n"
        "Arena is a rectangle centred on (0,0) with hard border walls.\n"
        "Movement is forward only along body heading; turning is rate-limited.\n"
        "fire() aims the turret toward the given direction but turret also turns rate-limited,\n"
        "so the actual shot direction is the CURRENT turret angle, not the requested angle.\n"
        "Projectiles spawn at each weapon's mount point on the turret.\n"
        "\n"
        "=== Python conventions you must follow ===\n"
        "1. This is Python, NOT Lua. Do NOT use 'local', 'then', 'end', '#', '--' comments.\n"
        "   Use Python syntax: assignments, 'if cond:', 'for x in list:', '#' for comments.\n"
        "2. Module-level mutable variables used inside think() require a 'global' declaration:\n"
        "     _fire_cd = 0.0                   # module level\n"
        "     def think(dt):\n"
        "         global _fire_cd              # required before assigning\n"
        "         _fire_cd -= dt\n"
        "   CRITICAL: if a variable is assigned ANYWHERE in a function (even conditionally),\n"
        "   Python treats it as local throughout that function. Always add it to the\n"
        "   'global' declaration at the top of think() if you assign to it inside think().\n"
        "3. Dict access for scan entries: t[\"type\"], t[\"x\"], t[\"z\"], t[\"distance\"], t[\"team\"].\n"
        "4. Use float('inf') for infinity; len(targets) for length; math.atan2(y, x) for angles.\n"
        "5. fire_weapon() uses 0-based index: fire_weapon(0, dx, dz) fires the first weapon.\n"
        "6. Never name a variable the same as an API function:\n"
        "     BAD:  scan = scan(0)       GOOD: targets = scan(0)\n"
        "     BAD:  move = (dx, dz)      GOOD: direction = (dx, dz)\n"
        "7. self_x, self_z, self_hp etc. are plain module globals injected each frame — read\n"
        "   them directly, never assign to them, no import needed.\n"
        "8. import math, import random are available; use them freely.\n"
        "9. NEVER use a local variable with the same name as a module-level variable unless\n"
        "   you add it to the global declaration. Example of the UnboundLocalError trap:\n"
        "     BAD:  def think(dt):\n"
        "               if condition:  _heading = new_val   # assignment makes it local\n"
        "               move(math.cos(_heading), ...)       # ERROR if condition was False\n"
        "     GOOD: def think(dt):\n"
        "               global _heading                     # declare global\n"
        "               if condition:  _heading = new_val   # now it is a global assignment\n"
        "               move(math.cos(_heading), ...)       # always reads the global\n"
        "\n"
        "=== Examples / cookbook block ===\n"
        "The current script ends with a long comment block titled\n"
        "  '# EXAMPLES / COOKBOOK ...'\n"
        "containing reference Python patterns (init shape, _wall_avoid,\n"
        "_nearest_enemy, fire patterns, range-tiered fire_weapon, common\n"
        "pitfalls). PRESERVE that whole comment block VERBATIM at the bottom of\n"
        "your output. It is the working set of patterns for the next iteration;\n"
        "deleting it costs you context on the next regeneration. Use those\n"
        "examples as the scaffolding for init()/think(); do not invent new\n"
        "untested patterns.\n"
        "\n"
        "=== Smoke-test combat check (post-generation validation) ===\n"
        "After generation we run the script in an isolated Python interpreter with stub\n"
        "move/fire/fire_weapon/scan, simulate 3.0 seconds (60 frames at dt=0.05s),\n"
        "and place 4 enemies at distance ~3.5u in the four cardinal directions\n"
        "(front +x, back -x, left +z, right -z) moving tangentially at varied speeds.\n"
        "self_hp is full for the first 1.5s and 10%% for the second 1.5s.\n"
        "The script PASSES only if at least ONE fire/fire_weapon call is aimed\n"
        "within 30 degrees of any enemy. To pass with zero ambiguity:\n"
        "  - in think(), call targets = scan(0)\n"
        "  - find any entry where t[\"type\"] == \"bot\" and t[\"team\"] != self_team\n"
        "  - call fire(t[\"x\"] - self_x, t[\"z\"] - self_z) every frame an enemy is in range\n"
        "Also: the smoke test loads the file BEFORE setting any self_* globals,\n"
        "so any file-scope code that reads self_x / self_z / self_team / self_hp\n"
        "will fail load. Keep file-scope code seedless or use time.time().\n"
        "\n"
        "=== Output format ===\n"
        "Return ONLY the Python script. No markdown fences, no commentary, no reasoning prose.\n"
        "The script must define init() returning a dict, and think(dt).\n"
        "Keep the EXAMPLES / COOKBOOK comment block at the bottom verbatim.\n");
}

typedef struct {
    char system_prompt[PROMPT_BUF_SIZE];
    char user_prompt[PROMPT_BUF_SIZE];
    char host[80];
    int  port;
    char script_path[256];
} ThreadArgs;

static void *generate_thread(void *arg);

static void launch_generation_thread(ThreadArgs *ta, const char *label) {
    pthread_t      tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    int prompt_len = (int)(strlen(ta->system_prompt) + strlen(ta->user_prompt));
    pthread_mutex_lock(&g_mutex);
    g_last_prompt_chars = prompt_len;
    pthread_mutex_unlock(&g_mutex);
    llm_bot_log(LLOG_NORM, ">> prompt %d chars  %s", prompt_len, label);

    printf("\n===== SYSTEM PROMPT TO LLM (%s) =====\n%s\n"
           "===== END SYSTEM PROMPT =====\n\n"
           "===== USER PROMPT TO LLM (%s) =====\n%s\n"
           "===== END USER PROMPT =====\n\n",
           label, ta->system_prompt, label, ta->user_prompt);
    fflush(stdout);

    if (pthread_create(&tid, &attr, generate_thread, ta) != 0) {
        llm_bot_log(LLOG_ERR, "!! pthread_create failed");
        free(ta);
        pthread_mutex_lock(&g_mutex);
        g_thread_busy = false;
        pthread_mutex_unlock(&g_mutex);
    }
    pthread_attr_destroy(&attr);
}

static void *generate_thread(void *arg) {
    ThreadArgs *ta = (ThreadArgs *)arg;

    char *response = (char *)malloc(RESPONSE_BUF_SIZE);
    char *python   = (char *)malloc(SCRIPT_BUF_SIZE);
    if (!response || !python) {
        free(response);
        free(python);
        free(ta);
        goto done;
    }

    llm_bot_log(LLOG_BRIGHT, ">> GEN start  %s:%d", ta->host, ta->port);

    char retry_user_prompt[PROMPT_BUF_SIZE];
    strncpy(retry_user_prompt, ta->user_prompt, sizeof(retry_user_prompt) - 1);
    retry_user_prompt[sizeof(retry_user_prompt) - 1] = '\0';

    bool success = false;
    char validation_err[512] = "";

    for (int attempt = 1; attempt <= 2 && !success; attempt++) {
        LlamaGenMeta meta;
        int rc = llama_generate(ta->host, ta->port,
                                ta->system_prompt,
                                retry_user_prompt,
                                response, RESPONSE_BUF_SIZE,
                                &meta);
        if (rc != 0 || strlen(response) < 10) {
            llm_bot_log(LLOG_ERR, "!! GEN failed (rc=%d)", rc);
            break;
        }

        int resp_len = (int)strlen(response);
        pthread_mutex_lock(&g_mutex);
        g_last_response_chars = resp_len;
        g_last_bytes_rx = resp_len * 8;
        strncpy(g_last_model, meta.model, sizeof(g_last_model) - 1);
        g_last_model[sizeof(g_last_model) - 1] = '\0';
        g_last_prompt_tokens     = meta.prompt_tokens;
        g_last_completion_tokens = meta.completion_tokens;
        g_last_total_tokens      = meta.total_tokens;
        pthread_mutex_unlock(&g_mutex);
        llm_bot_log(LLOG_NORM, "<< response %d chars (try %d/2)", resp_len, attempt);

        extract_python(response, python, SCRIPT_BUF_SIZE);

        /* ----------------------------------------------------------------
         * Validate the source IN MEMORY before writing to disk.
         * This prevents a bad script from overwriting a good one and being
         * loaded by the next match spawn before this thread finishes.
         * ---------------------------------------------------------------- */
        char static_hint[480] = "";
        detect_generated_bugs(python, static_hint, (int)sizeof(static_hint));

        if (!strstr(python, "def ")) {
            snprintf(validation_err, sizeof(validation_err),
                     "generated response did not contain Python function definitions");
            llm_bot_log(LLOG_ERR, "!! no def: %s", validation_err);
        } else if (static_hint[0] != '\0') {
            snprintf(validation_err, sizeof(validation_err),
                     "static check: %s", static_hint);
            llm_bot_log(LLOG_ERR, "!! static check: %s", static_hint);
        } else {
            /* Syntax check on the in-memory buffer */
            char syntax_err[512] = "";
            bool syntax_ok = false;
            {
                PyGILState_STATE gs = PyGILState_Ensure();
                PyObject *code = Py_CompileString(python, ta->script_path, Py_file_input);
                if (code) {
                    syntax_ok = true;
                    Py_DECREF(code);
                } else {
                    PyObject *et = NULL, *ev = NULL, *etb = NULL;
                    PyErr_Fetch(&et, &ev, &etb);
                    PyErr_NormalizeException(&et, &ev, &etb);
                    if (ev) {
                        PyObject *s = PyObject_Str(ev);
                        if (s) {
                            strncpy(syntax_err, PyUnicode_AsUTF8(s),
                                    sizeof(syntax_err) - 1);
                            Py_DECREF(s);
                        }
                    }
                    Py_XDECREF(et); Py_XDECREF(ev); Py_XDECREF(etb);
                }
                PyGILState_Release(gs);
            }

            if (!syntax_ok) {
                snprintf(validation_err, sizeof(validation_err), "%s", syntax_err);
                llm_bot_log(LLOG_ERR, "!! syntax ERR: %s", syntax_err);
            } else if (!smoke_test_source(python, validation_err,
                                          (int)sizeof(validation_err))) {
                llm_bot_log(LLOG_ERR, "!! smoke test failed");
                llm_bot_log(LLOG_ERR, "!! %s", validation_err);
            } else {
                /* All checks passed — check for stagnation (identical to current script) */
                char current_on_disk[SCRIPT_BUF_SIZE];
                bool is_stagnant = false;
                if (read_file(ta->script_path, current_on_disk, SCRIPT_BUF_SIZE) > 0
                    && strcmp(python, current_on_disk) == 0) {
                    is_stagnant = true;
                }

                if (is_stagnant && attempt < 2) {
                    /* Don't write — force a retry with an explicit change demand */
                    pthread_mutex_lock(&g_mutex);
                    g_stagnation_count++;
                    pthread_mutex_unlock(&g_mutex);
                    llm_bot_log(LLOG_WARN,
                                "!! stagnation #%d: generated script is identical to current — forcing retry",
                                g_stagnation_count);
                    snprintf(validation_err, sizeof(validation_err),
                             "stagnation: you generated the EXACT same script as the one already running. "
                             "You MUST make a genuinely different strategy. "
                             "Try a completely different movement pattern, firing logic, or body configuration.");
                } else {
                    /* Write to disk (even if stagnant on last attempt — better than nothing) */
                    if (write_file(ta->script_path, python) != 0) {
                        snprintf(validation_err, sizeof(validation_err),
                                 "write failed: %s", ta->script_path);
                    } else {
                        llm_bot_log(LLOG_OK, ">> syntax OK");
                        llm_bot_log(LLOG_OK, ">> smoke test OK");
                        if (is_stagnant) {
                            llm_bot_log(LLOG_WARN, ">> script unchanged after retry — keeping");
                        } else {
                            llm_bot_log(LLOG_OK, ">> script written (%d chars)", (int)strlen(python));
                            pthread_mutex_lock(&g_mutex);
                            g_stagnation_count = 0;
                            pthread_mutex_unlock(&g_mutex);
                        }

                        printf("\n===== GENERATED PYTHON SCRIPT (%d chars) =====\n%s\n"
                               "===== END GENERATED SCRIPT =====\n\n",
                               (int)strlen(python), python);
                        fflush(stdout);

                        pthread_mutex_lock(&g_mutex);
                        g_script_ready = true;
                        strncpy(g_script_status, "OK", sizeof(g_script_status) - 1);
                        g_script_color = LLOG_OK;
                        pthread_mutex_unlock(&g_mutex);
                        success = true;
                    }
                }
            }
        }

        if (!success && attempt < 2) {
            llm_bot_log(LLOG_WARN, "!! validation failed: %s", validation_err);
            llm_bot_log(LLOG_WARN, "!! retrying now");
            retry_user_prompt[0] = '\0';
            int retry_len = 0;
            append_text(retry_user_prompt, (int)sizeof(retry_user_prompt), &retry_len,
                        "%s\n", ta->user_prompt);
            append_text(retry_user_prompt, (int)sizeof(retry_user_prompt), &retry_len,
                        "=== IMMEDIATE VALIDATION FAILURE ===\n");
            append_text(retry_user_prompt, (int)sizeof(retry_user_prompt), &retry_len,
                        "Your previous generated script failed validation.\n");
            append_text(retry_user_prompt, (int)sizeof(retry_user_prompt), &retry_len,
                        "Exact error: %s\n", validation_err);
            append_text(retry_user_prompt, (int)sizeof(retry_user_prompt), &retry_len,
                        "Do NOT repeat the same construct. Regenerate the FULL script "
                        "from scratch, following the Python rules from the system prompt.\n");
        }
    }

    if (!success) {
        llm_bot_log(LLOG_ERR, "!! generation FAILED after retries: %s",
                    validation_err[0] ? validation_err : "generation failed");
        pthread_mutex_lock(&g_mutex);
        strncpy(g_gen_error, validation_err[0] ? validation_err : "generation failed",
                sizeof(g_gen_error) - 1);
        g_gen_error_pending = true;
        strncpy(g_script_status, "INVALID", sizeof(g_script_status) - 1);
        g_script_color = LLOG_ERR;
        pthread_mutex_unlock(&g_mutex);
    }

    free(response);
    free(python);
    free(ta);

done:
    pthread_mutex_lock(&g_mutex);
    g_thread_busy = false;
    bool have_pending = g_pending_match_valid;
    MatchStats pending = {0};
    if (have_pending) {
        pending = g_pending_match;
        g_pending_match_valid = false;
    }
    pthread_mutex_unlock(&g_mutex);

    if (have_pending) {
        llm_bot_log(LLOG_NORM, ">> flushing queued match %d", pending.match_number);
        llm_bot_submit_match(&pending);
    }
    return NULL;
}

/* ----------------------------------------------------------------------- */

void llm_bot_init(const char *host, int port, const char *script_path,
                  const char *user_prompt) {
    strncpy(g_host,        host,        sizeof(g_host)        - 1);
    g_port = port;
    strncpy(g_script_path, script_path, sizeof(g_script_path) - 1);
    if (user_prompt) {
        strncpy(g_user_prompt, user_prompt, sizeof(g_user_prompt) - 1);
        g_user_prompt[sizeof(g_user_prompt) - 1] = '\0';
    } else {
        g_user_prompt[0] = '\0';
    }
    g_script_ready = false;
    g_thread_busy  = false;
    g_last_model[0] = '\0';
    g_last_prompt_tokens = -1;
    g_last_completion_tokens = -1;
    g_last_total_tokens = -1;
    strncpy(g_script_status, "-", sizeof(g_script_status) - 1);
    g_script_color = LLOG_DIM;
    llm_bot_log(LLOG_DIM, "   server: %s:%d", host, port);
}

void llm_bot_set_user_prompt(const char *user_prompt) {
    pthread_mutex_lock(&g_mutex);
    if (user_prompt) {
        strncpy(g_user_prompt, user_prompt, sizeof(g_user_prompt) - 1);
        g_user_prompt[sizeof(g_user_prompt) - 1] = '\0';
    } else {
        g_user_prompt[0] = '\0';
    }
    pthread_mutex_unlock(&g_mutex);
}

static void start_generation(const MatchStats *s, bool push_history,
                             bool queue_if_busy) {
    pthread_mutex_lock(&g_mutex);
    bool busy = g_thread_busy;
    if (!busy) g_thread_busy = true;
    pthread_mutex_unlock(&g_mutex);

    if (busy) {
        if (queue_if_busy) {
            pthread_mutex_lock(&g_mutex);
            g_pending_match       = *s;
            g_pending_match_valid = true;
            pthread_mutex_unlock(&g_mutex);
            llm_bot_log(LLOG_WARN, "!! gen busy, queued gen %d", s->match_number);
        }
        return;
    }

    ThreadArgs *ta = (ThreadArgs *)malloc(sizeof(ThreadArgs));
    if (!ta) {
        pthread_mutex_lock(&g_mutex);
        g_thread_busy = false;
        pthread_mutex_unlock(&g_mutex);
        return;
    }

    strncpy(ta->host,        g_host,        sizeof(ta->host)        - 1);
    ta->port = g_port;
    strncpy(ta->script_path, g_script_path, sizeof(ta->script_path) - 1);

    char *current = (char *)malloc(SCRIPT_BUF_SIZE);
    if (!current) {
        free(ta);
        pthread_mutex_lock(&g_mutex);
        g_thread_busy = false;
        pthread_mutex_unlock(&g_mutex);
        return;
    }
    if (read_file(g_script_path, current, SCRIPT_BUF_SIZE) <= 0)
        snprintf(current, SCRIPT_BUF_SIZE, "# (script not found)\n");

    /* Push new generation result into rolling history (deploy boundaries only) */
    if (push_history) {
        if (g_match_history_count < MATCH_HISTORY_SIZE) {
            g_match_history[g_match_history_count++] = *s;
        } else {
            memmove(g_match_history, g_match_history + 1,
                    (MATCH_HISTORY_SIZE - 1) * sizeof(MatchStats));
            g_match_history[MATCH_HISTORY_SIZE - 1] = *s;
        }
    }

    /* Error section */
    char error_section[1024] = "";
    if (s->script_error[0] != '\0') {
        snprintf(error_section, sizeof(error_section),
            "=== SCRIPT ERROR — fix this before anything else! ===\n"
            "Python error: %s\n"
            "The bots had no AI this match because the script would not load.\n\n",
            s->script_error);
    } else if (s->runtime_error[0] != '\0') {
        snprintf(error_section, sizeof(error_section),
            "=== RUNTIME ERROR — fix this! ===\n"
            "Python error: %s\n"
            "The script loaded but crashed every frame during play, so bots did nothing.\n\n",
            s->runtime_error);
    }

    char known_bugs[2048] = "";
    build_known_bugs(current, known_bugs, (int)sizeof(known_bugs));

    char history_section[2048] = "";
    int  hs_len = 0;
    hs_len += snprintf(history_section + hs_len, (int)sizeof(history_section) - hs_len,
        "=== Recent generation results (%d generation(s)) ===\n"
        "Each entry is one deployed bot_llm generation, telemetry aggregated\n"
        "across ALL your bots over that generation's lifetime in the arena.\n",
        g_match_history_count);
    for (int hi = 0; hi < g_match_history_count; hi++) {
        const MatchStats *h = &g_match_history[hi];
        hs_len += snprintf(history_section + hs_len, (int)sizeof(history_section) - hs_len,
            "Gen %d: %.1fs | Survivors %d/%d | avgHP %.0f%% | Outcome: %s\n"
            "  Combat : shots_fired=%d  shots_hit=%d  hit_rate=%.1f%%  dmg=%.0f  kills=%d\n"
            "  Seeing : think_frames=%d  enemy_visible=%d (%.0f%%)  avg_nearest=%.2f u\n"
            "  Motion : arena_bumps=%d  wall_bumps=%d  fire_frames=%d\n",
            h->match_number,
            (double)h->duration,
            h->llm_survivors, h->llm_start,
            (double)(h->llm_avg_hp_frac * 100.0f),
            h->winner_name,
            h->shots_fired, h->shots_hit, (double)(h->hit_rate * 100.0f),
            (double)h->damage_dealt, h->kills,
            h->think_frames, h->enemy_visible_frames,
            (double)(h->visibility_frac * 100.0f),
            (double)h->avg_nearest_dist,
            h->arena_bumps, h->wall_bumps, h->fire_frames);
    }
    hs_len += snprintf(history_section + hs_len, (int)sizeof(history_section) - hs_len,
        "Interpretation hints:\n"
        "- hit_rate < 5%% => aim is bad or firing without a target; gate fire() on scan.\n"
        "- visibility < 20%% => rarely sees enemies; move toward centre, not walls.\n"
        "- arena_bumps >> 0 => wastes time ramming border; steer before reaching edge.\n"
        "- wall_bumps >> 0 => collides with internal walls; use wall scan entries to avoid them.\n"
        "- fire_frames ~ think_frames and hit_rate low => firing every frame into empty space.\n");

    build_system_prompt(ta->system_prompt, (int)sizeof(ta->system_prompt));

    /* Stagnation note: included when the model keeps regenerating the same script */
    char stagnation_note[512] = "";
    int stag = g_stagnation_count;
    if (stag >= 1) {
        snprintf(stagnation_note, sizeof(stagnation_note),
            "=== STAGNATION WARNING (repeat #%d) ===\n"
            "The last %d generation(s) produced a script IDENTICAL to what is already "
            "running. The score metrics in the match history below show the limits of "
            "the current approach. You MUST produce a genuinely different strategy: "
            "change the movement pattern, locomotion type, body, weapon choice, or "
            "firing logic in a meaningful way. Do NOT return the same code.\n\n",
            stag, stag);
    }

    snprintf(ta->user_prompt, sizeof(ta->user_prompt),
        "%s%s%s"
        "%s"
        "%s"
        "%s"
        "=== Current script ===\n"
        "%s\n"
        "\n"
        "%s"
        "\n"
        "Improve the script using the system rules above.\n",
        g_user_prompt[0] != '\0' ? "=== Extra user instructions ===\n" : "",
        g_user_prompt[0] != '\0' ? g_user_prompt : "",
        g_user_prompt[0] != '\0' ? "\n\n" : "",
        stagnation_note,
        known_bugs,
        error_section,
        current,
        history_section
    );

    free(current);

    {
        char label[64];
        snprintf(label, sizeof(label), "gen %d", s->match_number);
        launch_generation_thread(ta, label);
    }
}

void llm_bot_submit_match(const MatchStats *s) {
    start_generation(s, true, true);
}

void llm_bot_request_continue(void) {
    pthread_mutex_lock(&g_mutex);
    int count = g_match_history_count;
    MatchStats latest;
    memset(&latest, 0, sizeof(latest));
    if (count > 0) latest = g_match_history[count - 1];
    pthread_mutex_unlock(&g_mutex);

    if (count == 0) return;   /* nothing learned yet; wait for first deploy */
    start_generation(&latest, false, false);
}

void llm_bot_request_initial(void) {
    pthread_mutex_lock(&g_mutex);
    bool busy = g_thread_busy;
    if (!busy) g_thread_busy = true;
    pthread_mutex_unlock(&g_mutex);

    if (busy) {
        llm_bot_log(LLOG_WARN, "!! gen busy, skip initial bootstrap");
        return;
    }

    ThreadArgs *ta = (ThreadArgs *)malloc(sizeof(ThreadArgs));
    if (!ta) {
        pthread_mutex_lock(&g_mutex);
        g_thread_busy = false;
        pthread_mutex_unlock(&g_mutex);
        return;
    }

    strncpy(ta->host,        g_host,        sizeof(ta->host)        - 1);
    ta->port = g_port;
    strncpy(ta->script_path, g_script_path, sizeof(ta->script_path) - 1);

    char *current = (char *)malloc(SCRIPT_BUF_SIZE);
    if (!current) {
        free(ta);
        pthread_mutex_lock(&g_mutex);
        g_thread_busy = false;
        pthread_mutex_unlock(&g_mutex);
        return;
    }
    if (read_file(g_script_path, current, SCRIPT_BUF_SIZE) <= 0)
        snprintf(current, SCRIPT_BUF_SIZE, "# (script not found)\n");

    char known_bugs[2048] = "";
    build_known_bugs(current, known_bugs, (int)sizeof(known_bugs));
    build_system_prompt(ta->system_prompt, (int)sizeof(ta->system_prompt));

    snprintf(ta->user_prompt, sizeof(ta->user_prompt),
        "%s%s%s"
        "%s"
        "=== Current script ===\n"
        "%s\n"
        "\n"
        "=== Startup request ===\n"
        "Improve this script immediately before the arena starts.\n"
        "There is no history yet, so focus on producing a strong, stable,\n"
        "runtime-safe script that follows the system rules.\n"
        "This is a continuous arena: your bots respawn and self-improve forever.\n",
        g_user_prompt[0] != '\0' ? "=== Extra user instructions ===\n" : "",
        g_user_prompt[0] != '\0' ? g_user_prompt : "",
        g_user_prompt[0] != '\0' ? "\n\n" : "",
        known_bugs,
        current);

    free(current);
    launch_generation_thread(ta, "startup bootstrap");
}

void llm_bot_request_prompt_refresh(void) {
    pthread_mutex_lock(&g_mutex);
    bool busy = g_thread_busy;
    if (!busy) g_thread_busy = true;
    pthread_mutex_unlock(&g_mutex);

    if (busy) {
        llm_bot_log(LLOG_WARN, "!! gen busy, skip prompt refresh");
        return;
    }

    ThreadArgs *ta = (ThreadArgs *)malloc(sizeof(ThreadArgs));
    if (!ta) {
        pthread_mutex_lock(&g_mutex);
        g_thread_busy = false;
        pthread_mutex_unlock(&g_mutex);
        return;
    }

    strncpy(ta->host,        g_host,        sizeof(ta->host)        - 1);
    ta->port = g_port;
    strncpy(ta->script_path, g_script_path, sizeof(ta->script_path) - 1);

    char *current = (char *)malloc(SCRIPT_BUF_SIZE);
    if (!current) {
        free(ta);
        pthread_mutex_lock(&g_mutex);
        g_thread_busy = false;
        pthread_mutex_unlock(&g_mutex);
        return;
    }
    if (read_file(g_script_path, current, SCRIPT_BUF_SIZE) <= 0)
        snprintf(current, SCRIPT_BUF_SIZE, "# (script not found)\n");

    char known_bugs[2048] = "";
    build_known_bugs(current, known_bugs, (int)sizeof(known_bugs));
    build_system_prompt(ta->system_prompt, (int)sizeof(ta->system_prompt));

    snprintf(ta->user_prompt, sizeof(ta->user_prompt),
        "%s%s%s"
        "%s"
        "=== Current script ===\n"
        "%s\n"
        "\n"
        "=== Manual prompt refresh ===\n"
        "The user changed the extra instructions during the current session.\n"
        "Regenerate the full script immediately using the new user context.\n",
        g_user_prompt[0] != '\0' ? "=== Extra user instructions ===\n" : "",
        g_user_prompt[0] != '\0' ? g_user_prompt : "",
        g_user_prompt[0] != '\0' ? "\n\n" : "",
        known_bugs,
        current);

    free(current);
    launch_generation_thread(ta, "prompt refresh");
}

bool llm_bot_poll_ready(void) {
    pthread_mutex_lock(&g_mutex);
    bool ready = g_script_ready;
    if (ready) g_script_ready = false;
    pthread_mutex_unlock(&g_mutex);
    return ready;
}

bool llm_bot_poll_gen_error(char *buf, int size) {
    pthread_mutex_lock(&g_mutex);
    bool pending = g_gen_error_pending;
    if (pending) {
        if (buf && size > 0) {
            strncpy(buf, g_gen_error, size - 1);
            buf[size - 1] = '\0';
        }
        g_gen_error_pending = false;
        g_gen_error[0]      = '\0';
    }
    pthread_mutex_unlock(&g_mutex);
    return pending;
}

bool llm_bot_is_busy(void) {
    pthread_mutex_lock(&g_mutex);
    bool busy = g_thread_busy;
    pthread_mutex_unlock(&g_mutex);
    return busy;
}

void llm_bot_shutdown(void) {
    int retries = 50;
    while (retries-- > 0 && llm_bot_is_busy()) {
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 100000000L };
        nanosleep(&ts, NULL);
    }
}
