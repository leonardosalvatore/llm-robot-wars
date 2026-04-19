#include "llama_bot.h"
#include "llama_client.h"

#include <pthread.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdarg.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#define SCRIPT_BUF_SIZE   (32  * 1024)
#define RESPONSE_BUF_SIZE (128 * 1024)
#define PROMPT_BUF_SIZE   (24  * 1024)

/* ----------------------------------------------------------------------- */
static char g_host[80]         = {0};
static int  g_port             = 8080;
static char g_script_path[256] = {0};
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

/* Single-slot queue: if a match ends while a generation is still running, we
 * stash its stats here and submit it as soon as the generation thread exits.
 * Keeping only the newest guarantees the LLM always sees the latest match. */
static bool       g_pending_match_valid = false;
static MatchStats g_pending_match;

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
static void extract_lua(const char *response, char *out, int out_size) {
    const char *start = strstr(response, "```lua");
    if (start) {
        start += 6;
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
        /* Opening fence found but no closing fence (truncated response).
         * Use everything after the opening fence anyway. */
        int len = (int)strlen(start);
        if (len >= out_size) len = out_size - 1;
        while (len > 0 && (start[len-1] == '\n' || start[len-1] == '\r'
                            || start[len-1] == ' '))
            len--;
        memcpy(out, start, (size_t)len);
        out[len] = '\0';
        return;
    }

    /* No fences at all -- strip any leading backticks just in case */
    const char *p = response;
    while (*p == '`') p++;
    while (*p == '\n' || *p == '\r') p++;
    /* Strip bare language-hint line ("lua\n") that some models emit
     * instead of a proper ```lua fence */
    if (strncmp(p, "lua\n", 4) == 0)        p += 4;
    else if (strncmp(p, "lua\r\n", 5) == 0) p += 5;
    int len = (int)strlen(p);
    if (len >= out_size) len = out_size - 1;
    memcpy(out, p, (size_t)len);
    out[len] = '\0';
}

/* ----------------------------------------------------------------------- */
static int smoke_move(lua_State *L) {
    (void)L;
    return 0;
}

static int smoke_fire(lua_State *L) {
    (void)L;
    return 0;
}

static int smoke_scan(lua_State *L) {
    (void)luaL_optnumber(L, 1, 18.0);

    lua_newtable(L);

    lua_newtable(L);
    lua_pushstring(L, "bot"); lua_setfield(L, -2, "type");
    lua_pushnumber(L, 3.0);   lua_setfield(L, -2, "x");
    lua_pushnumber(L, 2.0);   lua_setfield(L, -2, "z");
    lua_pushnumber(L, 3.6);   lua_setfield(L, -2, "distance");
    lua_pushinteger(L, 1);    lua_setfield(L, -2, "team");
    lua_rawseti(L, -2, 1);

    lua_newtable(L);
    lua_pushstring(L, "wall"); lua_setfield(L, -2, "type");
    lua_pushnumber(L, -1.0);   lua_setfield(L, -2, "x");
    lua_pushnumber(L,  0.5);   lua_setfield(L, -2, "z");
    lua_pushnumber(L,  1.2);   lua_setfield(L, -2, "distance");
    lua_rawseti(L, -2, 2);

    return 1;
}

static void smoke_set_globals(lua_State *L, double x, double z,
                              double hp, double max_hp) {
    lua_pushnumber(L, x);      lua_setglobal(L, "self_x");
    lua_pushnumber(L, z);      lua_setglobal(L, "self_z");
    lua_pushnumber(L, x);      lua_setglobal(L, "self_last_x");
    lua_pushnumber(L, z);      lua_setglobal(L, "self_last_z");
    lua_pushinteger(L, 6);     lua_setglobal(L, "self_team");
    lua_pushnumber(L, hp);     lua_setglobal(L, "self_hp");
    lua_pushnumber(L, max_hp); lua_setglobal(L, "self_max_hp");
}

static bool smoke_test_script(const char *path, char *err, int err_size) {
    lua_State *L = luaL_newstate();
    if (!L) {
        snprintf(err, (size_t)err_size, "smoke test: failed to create Lua state");
        return false;
    }

    luaL_openlibs(L);
    lua_register(L, "move", smoke_move);
    lua_register(L, "fire", smoke_fire);
    lua_register(L, "scan", smoke_scan);

    smoke_set_globals(L, 0.0, 0.0, 180.0, 250.0);

    if (luaL_dofile(L, path) != LUA_OK) {
        const char *msg = lua_tostring(L, -1);
        snprintf(err, (size_t)err_size, "smoke test load error: %s",
                 msg ? msg : "(unknown error)");
        lua_close(L);
        return false;
    }

    lua_getglobal(L, "init");
    if (lua_type(L, -1) != LUA_TFUNCTION) {
        snprintf(err, (size_t)err_size, "smoke test: init() missing");
        lua_close(L);
        return false;
    }
    if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
        const char *msg = lua_tostring(L, -1);
        snprintf(err, (size_t)err_size, "smoke test init() error: %s",
                 msg ? msg : "(unknown error)");
        lua_close(L);
        return false;
    }
    if (lua_type(L, -1) != LUA_TTABLE) {
        snprintf(err, (size_t)err_size, "smoke test: init() must return a table");
        lua_close(L);
        return false;
    }
    lua_pop(L, 1);

    smoke_set_globals(L, 0.0, 0.0, 180.0, 250.0);
    lua_getglobal(L, "think");
    if (lua_type(L, -1) != LUA_TFUNCTION) {
        snprintf(err, (size_t)err_size, "smoke test: think() missing");
        lua_close(L);
        return false;
    }
    lua_pushnumber(L, 0.016);
    if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
        const char *msg = lua_tostring(L, -1);
        snprintf(err, (size_t)err_size, "smoke test think() error: %s",
                 msg ? msg : "(unknown error)");
        lua_close(L);
        return false;
    }

    smoke_set_globals(L, 0.2, -0.1, 40.0, 250.0);
    lua_getglobal(L, "think");
    if (lua_type(L, -1) != LUA_TFUNCTION) {
        snprintf(err, (size_t)err_size, "smoke test: think() missing on second pass");
        lua_close(L);
        return false;
    }
    lua_pushnumber(L, 0.033);
    if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
        const char *msg = lua_tostring(L, -1);
        snprintf(err, (size_t)err_size, "smoke test think() error: %s",
                 msg ? msg : "(unknown error)");
        lua_close(L);
        return false;
    }

    lua_close(L);
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
/* Detect patterns in a GENERATED script that we know break at runtime.
 * Returns the first issue as a plain-text hint, or empty if clean. */
static void detect_generated_bugs(const char *script, char *hint, int hint_size) {
    hint[0] = '\0';
    if (!script) return;

    if (strstr(script, "math.atan2")) {
        snprintf(hint, (size_t)hint_size,
            "Your script uses math.atan2, which does not exist in Lua 5.3+. "
            "Replace every math.atan2(y, x) with math.atan(y, x).");
        return;
    }
    if (strstr(script, "math.hypot")) {
        snprintf(hint, (size_t)hint_size,
            "Your script uses math.hypot, which does not exist. "
            "Replace with math.sqrt(x*x + y*y).");
        return;
    }
    /* Table-literal subscript: "}[" after a closing brace that is part of a table
     * literal is almost always the broken `{...}[key]` pattern. */
    {
        const char *p = script;
        while ((p = strstr(p, "}[")) != NULL) {
            /* Walk backwards to find the matching '{' and confirm it's a literal,
             * not a function/body. A function body would have ")" or "end" before. */
            int depth = 1;
            const char *q = p - 1;
            while (q > script && depth > 0) {
                if (*q == '}') depth++;
                else if (*q == '{') depth--;
                if (depth == 0) break;
                q--;
            }
            if (depth == 0 && q > script) {
                /* Scan left of '{' for non-space; if it's '=' or ',' or '(' or 'return'
                 * we're subscripting a table literal. */
                const char *r = q - 1;
                while (r > script && (*r == ' ' || *r == '\t' || *r == '\n')) r--;
                if (*r == '=' || *r == ',' || *r == '(' ||
                    (r >= script + 5 && strncmp(r - 5, "return", 6) == 0)) {
                    snprintf(hint, (size_t)hint_size,
                        "Your script subscripts a table literal in one expression "
                        "(pattern '{...}[key]'). Lua parses this as two statements "
                        "and crashes. Assign the table to a local first: "
                        "local T = {...}; local v = T[key].");
                    return;
                }
            }
            p += 2;
        }
    }
    /* init() called at file scope before think definitions run. Typical
     * telltale: a top-level call "init()" that is NOT inside a function body. */
    {
        const char *p = script;
        while ((p = strstr(p, "init()")) != NULL) {
            /* Skip the definition line. */
            if (p >= script + 9 && strncmp(p - 9, "function ", 9) == 0) { p += 6; continue; }
            /* Check whether this occurrence is inside any function by counting
             * "function" vs "end" tokens before it. */
            int fn_count = 0, end_count = 0;
            for (const char *s = script; s < p; s++) {
                if (strncmp(s, "function", 8) == 0 &&
                    (s == script || !(s[-1] >= 'a' && s[-1] <= 'z')) &&
                    !(s[8] >= 'a' && s[8] <= 'z')) fn_count++;
                if (strncmp(s, "end", 3) == 0 &&
                    (s == script || !(s[-1] >= 'a' && s[-1] <= 'z')) &&
                    !(s[3] >= 'a' && s[3] <= 'z')) end_count++;
            }
            if (fn_count <= end_count) {
                snprintf(hint, (size_t)hint_size,
                    "Your script calls init() at file scope (outside any function). "
                    "Do not call init() yourself -- the engine calls it. "
                    "Move any data you need out of init() into a plain local table.");
                return;
            }
            p += 6;
        }
    }
}

static void build_known_bugs(const char *current, char *known_bugs, int size) {
    int kb_len = 0;
    known_bugs[0] = '\0';

    if (strstr(current, "local scan") != NULL) {
        kb_len += snprintf(known_bugs + kb_len, (size_t)(size - kb_len),
            "=== CRITICAL BUG: variable shadows scan() API ===\n"
            "The script uses `local scan = scan(...)` which SHADOWS the global\n"
            "scan() API function. Every subsequent call to scan() then fails.\n"
            "Fix: use a different name, e.g. `local targets = scan(radius)`.\n"
            "NEVER name a local variable the same as an API function.\n"
            "\n");
    }

    if (strstr(current, "math.atan2") != NULL) {
        kb_len += snprintf(known_bugs + kb_len, (size_t)(size - kb_len),
            "=== CRITICAL BUG: math.atan2 does not exist in Lua 5.3+ ===\n"
            "Replace every `math.atan2(y, x)` with `math.atan(y, x)`.\n"
            "In Lua 5.3+, math.atan accepts two arguments and replaces math.atan2.\n"
            "\n");
    }

    if (strstr(current, "math.hypot") != NULL) {
        kb_len += snprintf(known_bugs + kb_len, (size_t)(size - kb_len),
            "=== CRITICAL BUG: math.hypot does not exist in Lua 5.3+ ===\n"
            "Replace `math.hypot(x, y)` with `math.sqrt(x*x + y*y)`.\n"
            "Do not call math.hypot in this environment.\n"
            "\n");
    }
}

static void build_system_prompt(char *dst, int dst_size) {
    snprintf(dst, (size_t)dst_size,
        "You are iteratively improving a Lua script for an arena combat robot game.\n"
        "\n"
        "=== Game API ===\n"
        "move(dx, dz)   -- set movement direction; internally normalised; magnitude ignored.\n"
        "               -- Do NOT multiply dx/dz by a speed scalar -- pass a unit direction.\n"
        "fire(dx, dz)   -- aim turret toward (dx,dz) and fire BOTH weapons this frame.\n"
        "               -- There is no internal cooldown; call from your own timer.\n"
        "scan(radius)   -- radius argument is ignored; returns all bots (LOS) + walls.\n"
        "               -- entries: {type=\"bot\",  x, z, distance, team, hp, max_hp}\n"
        "                           {type=\"wall\", x, z, distance}   (x,z = nearest point)\n"
        "Per-frame globals:\n"
        "  self_x, self_z        -- world position (z is the forward axis)\n"
        "  self_team             -- integer script id of this bot's team\n"
        "  self_hp, self_max_hp  -- current and maximum hit points\n"
        "  self_left_weapon, self_right_weapon  -- string, same values as init()\n"
        "  self_armour           -- integer 0..3\n"
        "  self_max_speed        -- current max linear speed in units/second\n"
        "init() must return: {left_weapon=..., right_weapon=..., armour=...}\n"
        "\n"
        "=== Weapon stats (per projectile; both weapons fire together on fire()) ===\n"
        "                 damage  speed(u/s)  lifetime(s)  range  turret_turn(rad/s)  fire_interval(s)\n"
        "  MachineGun       5      20            3.0         60          8                0.12\n"
        "  AutoCannon      25      15            6.0         90          4                0.60\n"
        "  Laser            2      90            1.0         90          2                0.08\n"
        "Hit radius on target is ~0.6 units. No projectile drop.\n"
        "fire() is engine-rate-limited per weapon: if its cooldown has not elapsed the\n"
        "shot is silently dropped. You can safely call fire() every frame; excess calls\n"
        "cost nothing. But to aim better, still gate fire() on having a target in range.\n"
        "\n"
        "=== Armour / chassis stats (integer armour 0..3) ===\n"
        "                 max_hp  max_speed(u/s)  body_turn(rad/s)  body_scale\n"
        "  armour 0        100     5.0             10                0.7\n"
        "  armour 1        150     3.5              6                1.0\n"
        "  armour 2        200     2.0              3.5              1.3\n"
        "  armour 3        250     0.8              2.0              1.6\n"
        "Turret turn rate uses the SLOWER of the two weapons' rates.\n"
        "\n"
        "=== Arena & physics ===\n"
        "Arena is a rectangle centred on (0,0) with hard border walls.\n"
        "Movement is forward only along body heading; turning is rate-limited.\n"
        "fire() aims the turret toward the given direction but turret also turns rate-limited,\n"
        "so the actual shot direction is the CURRENT turret angle, not the requested angle.\n"
        "Projectiles come from the two weapon mounts on either side of the body.\n"
        "\n"
        "=== Lua 5.4 quirks you keep getting wrong ===\n"
        "1. math.atan2 does NOT exist. Use math.atan(y, x).\n"
        "2. math.hypot does NOT exist. Use math.sqrt(x*x + y*y).\n"
        "3. You cannot subscript a table literal in one expression:\n"
        "     BAD:  local d = {a=1, b=2}[key]\n"
        "     GOOD: local T = {a=1, b=2}; local d = T[key]\n"
        "4. Do NOT call functions at file scope before they are defined later in the file.\n"
        "   Globals are resolved at call time but the function must exist by then. Safer:\n"
        "   declare tables/constants at top and functions after, call functions only from think().\n"
        "5. Every local variable name must differ from API names (move, fire, scan).\n"
        "     BAD:  local scan = scan(r)\n"
        "     GOOD: local targets = scan(0)\n"
        "6. Lua has no continue keyword. Use `goto continue` with a label, or nest an if.\n"
        "\n"
        "=== Output format ===\n"
        "Return ONLY the Lua script. No markdown fences, no commentary, no reasoning prose.\n"
        "The script must define init() returning a table, and think(dt).\n");
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
    char *lua      = (char *)malloc(SCRIPT_BUF_SIZE);
    if (!response || !lua) {
        free(response);
        free(lua);
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
        g_last_prompt_tokens = meta.prompt_tokens;
        g_last_completion_tokens = meta.completion_tokens;
        g_last_total_tokens = meta.total_tokens;
        pthread_mutex_unlock(&g_mutex);
        llm_bot_log(LLOG_NORM, "<< response %d chars (try %d/2)", resp_len, attempt);

        extract_lua(response, lua, SCRIPT_BUF_SIZE);

    char static_hint[480] = "";
    detect_generated_bugs(lua, static_hint, (int)sizeof(static_hint));

        if (!strstr(lua, "function")) {
            snprintf(validation_err, sizeof(validation_err),
                     "generated response did not contain Lua functions");
        } else if (static_hint[0] != '\0') {
            snprintf(validation_err, sizeof(validation_err),
                     "static check: %s", static_hint);
        } else if (write_file(ta->script_path, lua) != 0) {
            snprintf(validation_err, sizeof(validation_err),
                     "write failed: %s", ta->script_path);
        } else {
            llm_bot_log(LLOG_OK, ">> script written (%d chars)", (int)strlen(lua));

            printf("\n===== GENERATED LUA SCRIPT (%d chars) =====\n%s\n"
                   "===== END GENERATED SCRIPT =====\n\n",
                   (int)strlen(lua), lua);
            fflush(stdout);

            char lua_err[512] = "";
            bool syntax_ok    = false;
            lua_State *LS = luaL_newstate();
            if (LS) {
                syntax_ok = (luaL_loadfile(LS, ta->script_path) == LUA_OK);
                if (!syntax_ok) {
                    const char *msg = lua_tostring(LS, -1);
                    if (msg) strncpy(lua_err, msg, sizeof(lua_err) - 1);
                }
                lua_close(LS);
            } else {
                syntax_ok = true;
            }

            if (!syntax_ok) {
                snprintf(validation_err, sizeof(validation_err), "%s", lua_err);
                const char *colon = strrchr(lua_err, ':');
                llm_bot_log(LLOG_ERR, "!! lua ERR:%s", colon ? colon + 1 : lua_err);
            } else if (!smoke_test_script(ta->script_path, validation_err,
                                          (int)sizeof(validation_err))) {
                llm_bot_log(LLOG_ERR, "!! smoke test failed");
                llm_bot_log(LLOG_ERR, "!! %s", validation_err);
            } else {
                llm_bot_log(LLOG_OK, ">> lua syntax OK");
                llm_bot_log(LLOG_OK, ">> smoke test OK");
                pthread_mutex_lock(&g_mutex);
                g_script_ready = true;
                strncpy(g_script_status, "OK", sizeof(g_script_status) - 1);
                g_script_color = LLOG_OK;
                pthread_mutex_unlock(&g_mutex);
                success = true;
                break;
            }
        }

        if (attempt < 2) {
            llm_bot_log(LLOG_WARN, "!! validation failed, retrying now");
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
                        "from scratch, applying the Lua quirks rules from the system prompt.\n");
            /* NOTE: intentionally do NOT include the broken script -- past runs show
             * the model re-emits the same invalid pattern when it sees it again. */
        }
    }

    if (!success) {
        pthread_mutex_lock(&g_mutex);
        strncpy(g_gen_error, validation_err[0] ? validation_err : "generation failed",
                sizeof(g_gen_error) - 1);
        g_gen_error_pending = true;
        strncpy(g_script_status, "INVALID", sizeof(g_script_status) - 1);
        g_script_color = LLOG_ERR;
        pthread_mutex_unlock(&g_mutex);
    }

    free(response);
    free(lua);
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

void llm_bot_submit_match(const MatchStats *s) {
    pthread_mutex_lock(&g_mutex);
    bool busy = g_thread_busy;
    if (!busy) g_thread_busy = true;
    pthread_mutex_unlock(&g_mutex);

    if (busy) {
        pthread_mutex_lock(&g_mutex);
        g_pending_match       = *s;
        g_pending_match_valid = true;
        pthread_mutex_unlock(&g_mutex);
        llm_bot_log(LLOG_WARN, "!! gen busy, queued match %d", s->match_number);
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
        snprintf(current, SCRIPT_BUF_SIZE, "-- (script not found)\n");

    /* Push new match into rolling history */
    if (g_match_history_count < MATCH_HISTORY_SIZE) {
        g_match_history[g_match_history_count++] = *s;
    } else {
        memmove(g_match_history, g_match_history + 1,
                (MATCH_HISTORY_SIZE - 1) * sizeof(MatchStats));
        g_match_history[MATCH_HISTORY_SIZE - 1] = *s;
    }

    /* Error section: load-time errors take priority; runtime errors otherwise */
    char error_section[1024] = "";
    if (s->script_error[0] != '\0') {
        snprintf(error_section, sizeof(error_section),
            "=== SCRIPT ERROR — fix this before anything else! ===\n"
            "Lua error: %s\n"
            "The bots had no AI this match because the script would not load.\n"
            "\n",
            s->script_error);
    } else if (s->runtime_error[0] != '\0') {
        snprintf(error_section, sizeof(error_section),
            "=== RUNTIME ERROR — fix this! ===\n"
            "Lua error: %s\n"
            "The script loaded but crashed every frame during play, so bots did nothing.\n"
            "\n",
            s->runtime_error);
    }

    /* Known bugs detected by static analysis of the current script */
    char known_bugs[2048] = "";
    build_known_bugs(current, known_bugs, (int)sizeof(known_bugs));

    /* Build compact match-history section (last 1-3 matches) */
    char history_section[2048] = "";
    int  hs_len = 0;
    hs_len += snprintf(history_section + hs_len, (int)sizeof(history_section) - hs_len,
        "=== Recent match results (%d match(es)) ===\n"
        "Telemetry is aggregated across ALL your bots for that match.\n",
        g_match_history_count);
    for (int hi = 0; hi < g_match_history_count; hi++) {
        const MatchStats *h = &g_match_history[hi];
        hs_len += snprintf(history_section + hs_len, (int)sizeof(history_section) - hs_len,
            "Match %d/%d: %.1fs | Survivors %d/%d | avgHP %.0f%% | Winner: %s\n"
            "  Combat : shots_fired=%d  shots_hit=%d  hit_rate=%.1f%%  dmg=%.0f  kills=%d\n"
            "  Seeing : think_frames=%d  enemy_visible=%d (%.0f%%)  avg_nearest=%.2f u\n"
            "  Motion : arena_bumps=%d  wall_bumps=%d  fire_frames=%d\n",
            h->match_number, h->total_matches,
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

    snprintf(ta->user_prompt, sizeof(ta->user_prompt),
        "%s%s%s"
        "%s"   /* known_bugs */
        "%s"   /* error_section */
        "=== Current script ===\n"
        "%s\n"
        "\n"
        "%s"   /* history_section */
        "\n"
        "Improve the script using the system rules above.\n",
        g_user_prompt[0] != '\0' ? "=== Extra user instructions ===\n" : "",
        g_user_prompt[0] != '\0' ? g_user_prompt : "",
        g_user_prompt[0] != '\0' ? "\n\n" : "",
        known_bugs,
        error_section,
        current,
        history_section
    );

    free(current);

    {
        char label[64];
        snprintf(label, sizeof(label), "match %d/%d",
                 s->match_number, s->total_matches);
        launch_generation_thread(ta, label);
    }
}

void llm_bot_request_initial(int total_matches) {
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
        snprintf(current, SCRIPT_BUF_SIZE, "-- (script not found)\n");

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
        "Improve this script immediately before the first match starts.\n"
        "There is no match history yet, so focus on producing a strong, stable,\n"
        "runtime-safe script that follows the system rules.\n"
        "Total planned matches: %d\n",
        g_user_prompt[0] != '\0' ? "=== Extra user instructions ===\n" : "",
        g_user_prompt[0] != '\0' ? g_user_prompt : "",
        g_user_prompt[0] != '\0' ? "\n\n" : "",
        known_bugs,
        current,
        total_matches);

    free(current);
    launch_generation_thread(ta, "startup bootstrap");
}

void llm_bot_request_prompt_refresh(int total_matches) {
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
        snprintf(current, SCRIPT_BUF_SIZE, "-- (script not found)\n");

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
        "Regenerate the full script immediately using the new user context.\n"
        "Total planned matches: %d\n",
        g_user_prompt[0] != '\0' ? "=== Extra user instructions ===\n" : "",
        g_user_prompt[0] != '\0' ? g_user_prompt : "",
        g_user_prompt[0] != '\0' ? "\n\n" : "",
        known_bugs,
        current,
        total_matches);

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
