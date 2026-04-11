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

#define SCRIPT_BUF_SIZE   (32  * 1024)
#define RESPONSE_BUF_SIZE (128 * 1024)
#define PROMPT_BUF_SIZE   (24  * 1024)

/* ----------------------------------------------------------------------- */
static char g_host[80]         = {0};
static int  g_port             = 8080;
static char g_script_path[256] = {0};

static pthread_mutex_t g_mutex        = PTHREAD_MUTEX_INITIALIZER;
static bool            g_thread_busy  = false;
static bool            g_script_ready = false;

static LlmLogLine    g_vis_log[LLM_VIS_LOG_LINES];
static int            g_vis_log_head        = 0;
static int            g_vis_log_count       = 0;
static int            g_last_prompt_chars   = 0;
static int            g_last_response_chars = 0;
static int            g_last_bytes_rx       = 0;
static char           g_script_status[72]   = "-";
static LlmLogColor    g_script_color        = LLOG_DIM;
static char           g_gen_error[512]      = "";
static bool           g_gen_error_pending   = false;

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
    int len = (int)strlen(p);
    if (len >= out_size) len = out_size - 1;
    memcpy(out, p, (size_t)len);
    out[len] = '\0';
}

/* ----------------------------------------------------------------------- */
typedef struct {
    char prompt[PROMPT_BUF_SIZE];
    char host[80];
    int  port;
    char script_path[256];
} ThreadArgs;

static void *generate_thread(void *arg) {
    ThreadArgs *ta = (ThreadArgs *)arg;

    char *response = (char *)malloc(RESPONSE_BUF_SIZE);
    if (!response) { free(ta); goto done; }

    llm_bot_log(LLOG_BRIGHT, ">> GEN start  %s:%d", ta->host, ta->port);

    int rc = llama_generate(ta->host, ta->port,
                            ta->prompt,
                            response, RESPONSE_BUF_SIZE);
    if (rc != 0 || strlen(response) < 10) {
        llm_bot_log(LLOG_ERR, "!! GEN failed (rc=%d)", rc);
        free(response);
        free(ta);
        goto done;
    }

    int resp_len = (int)strlen(response);
    pthread_mutex_lock(&g_mutex);
    g_last_response_chars = resp_len;
    g_last_bytes_rx = resp_len * 8;
    pthread_mutex_unlock(&g_mutex);
    llm_bot_log(LLOG_NORM, "<< response %d chars", resp_len);

    char *lua = (char *)malloc(SCRIPT_BUF_SIZE);
    if (!lua) { free(response); free(ta); goto done; }

    extract_lua(response, lua, SCRIPT_BUF_SIZE);
    free(response);

    if (!strstr(lua, "function")) {
        llm_bot_log(LLOG_ERR, "!! no Lua functions found, discarding");
        pthread_mutex_lock(&g_mutex);
        strncpy(g_script_status, "NO FUNCTIONS", sizeof(g_script_status) - 1);
        g_script_color = LLOG_ERR;
        pthread_mutex_unlock(&g_mutex);
        free(lua);
        free(ta);
        goto done;
    }

    if (write_file(ta->script_path, lua) == 0) {
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

        if (syntax_ok) {
            llm_bot_log(LLOG_OK, ">> lua syntax OK");
            pthread_mutex_lock(&g_mutex);
            g_script_ready = true;
            strncpy(g_script_status, "OK", sizeof(g_script_status) - 1);
            g_script_color = LLOG_OK;
            pthread_mutex_unlock(&g_mutex);
        } else {
            const char *colon = strrchr(lua_err, ':');
            llm_bot_log(LLOG_ERR, "!! lua ERR:%s", colon ? colon + 1 : lua_err);
            pthread_mutex_lock(&g_mutex);
            strncpy(g_gen_error, lua_err, sizeof(g_gen_error) - 1);
            g_gen_error_pending = true;
            strncpy(g_script_status, "INVALID", sizeof(g_script_status) - 1);
            g_script_color = LLOG_ERR;
            pthread_mutex_unlock(&g_mutex);
        }
    } else {
        llm_bot_log(LLOG_ERR, "!! write failed: %s", ta->script_path);
    }

    free(lua);
    free(ta);

done:
    pthread_mutex_lock(&g_mutex);
    g_thread_busy = false;
    pthread_mutex_unlock(&g_mutex);
    return NULL;
}

/* ----------------------------------------------------------------------- */

void llm_bot_init(const char *host, int port, const char *script_path) {
    strncpy(g_host,        host,        sizeof(g_host)        - 1);
    g_port = port;
    strncpy(g_script_path, script_path, sizeof(g_script_path) - 1);
    g_script_ready = false;
    g_thread_busy  = false;
    strncpy(g_script_status, "-", sizeof(g_script_status) - 1);
    g_script_color = LLOG_DIM;
    llm_bot_log(LLOG_DIM, "   server: %s:%d", host, port);
}

void llm_bot_submit_match(const MatchStats *s) {
    pthread_mutex_lock(&g_mutex);
    bool busy = g_thread_busy;
    if (!busy) g_thread_busy = true;
    pthread_mutex_unlock(&g_mutex);

    if (busy) {
        llm_bot_log(LLOG_WARN, "!! gen busy, skip match %d", s->match_number);
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

    char error_section[1024] = "";
    if (s->script_error[0] != '\0') {
        snprintf(error_section, sizeof(error_section),
            "=== SCRIPT ERROR — fix this before anything else! ===\n"
            "Lua error: %s\n"
            "The bots had no AI this match because the script would not load.\n"
            "\n",
            s->script_error);
    }

    /* Detect known bug: shadowing the scan() API with a local variable.
     * e.g.  local scan = scan(r)   -- shadows the global scan function! */
    char known_bugs[512] = "";
    if (strstr(current, "local scan") != NULL) {
        snprintf(known_bugs, sizeof(known_bugs),
            "=== CRITICAL BUG IN CURRENT SCRIPT ===\n"
            "The script uses `local scan = scan(...)` which SHADOWS the global\n"
            "scan() API function. Every subsequent call to scan() then fails with\n"
            "\"attempt to call a table value (local 'scan')\".\n"
            "Fix: store scan results in a DIFFERENT variable name, e.g.:\n"
            "  local targets = scan(radius)\n"
            "  for _, t in ipairs(targets) do ... end\n"
            "Never name a local variable the same as an API function.\n"
            "\n");
    }

    snprintf(ta->prompt, sizeof(ta->prompt),
        "You are iteratively improving a Lua script for an arena combat robot game.\n"
        "\n"
        "=== Game API ===\n"
        "move(dx, dz)   -- set movement direction (internally normalised)\n"
        "fire(dx, dz)   -- aim and fire weapons in direction (dx, dz)\n"
        "scan(radius)   -- returns table of entries:\n"
        "                  {type=\"bot\",  x, z, distance, team}  -- enemy/ally\n"
        "                  {type=\"wall\", x, z, distance}        -- wall surface\n"
        "Per-frame globals: self_x, self_z, self_team, self_hp, self_max_hp\n"
        "Inertia: heavier armour slows hull turning; heavier weapons slow turret aim.\n"
        "init() must return: {left_weapon, right_weapon, armour}\n"
        "  Weapons: \"MachineGun\" | \"AutoCannon\" | \"Laser\"\n"
        "  Armour:  0 (fast, 100 HP) ... 3 (slow, 250 HP)\n"
        "\n"
        "=== NAMING RULES (CRITICAL) ===\n"
        "NEVER name a local variable the same as an API function.\n"
        "BAD:  local scan = scan(r)   -- shadows scan(), breaks all future calls\n"
        "GOOD: local targets = scan(r)\n"
        "Same rule applies to move, fire, and any other API name.\n"
        "\n"
        "%s"   /* known_bugs */
        "%s"   /* error_section */
        "=== Current script ===\n"
        "%s\n"
        "\n"
        "=== Match %d / %d result ===\n"
        "Duration  : %.1f s\n"
        "Survivors : %d / %d started  (avg HP %.0f%%)\n"
        "Damage    : %.0f dealt  |  %d kills\n"
        "Winner    : %s\n"
        "\n"
        "Return ONLY the improved Lua script, no explanation, no markdown fences.\n",
        known_bugs,
        error_section,
        current,
        s->match_number, s->total_matches,
        (double)s->duration,
        s->llm_survivors, s->llm_start,
        (double)(s->llm_avg_hp_frac * 100.0f),
        (double)s->damage_dealt, s->kills,
        s->winner_name
    );

    free(current);

    pthread_t      tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    int prompt_len = (int)strlen(ta->prompt);
    pthread_mutex_lock(&g_mutex);
    g_last_prompt_chars = prompt_len;
    pthread_mutex_unlock(&g_mutex);
    llm_bot_log(LLOG_NORM, ">> prompt %d chars  match %d/%d",
                   prompt_len, s->match_number, s->total_matches);

    printf("\n===== PROMPT TO LLM (match %d/%d, %d chars) =====\n%s\n"
           "===== END PROMPT =====\n\n",
           s->match_number, s->total_matches, prompt_len, ta->prompt);
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
