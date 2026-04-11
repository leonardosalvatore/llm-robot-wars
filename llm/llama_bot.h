#pragma once
#include <stdbool.h>
#include <stdarg.h>

#define LLM_VIS_LOG_LINES 28
#define LLM_VIS_LOG_COLS  54

typedef enum {
    LLOG_DIM = 0,
    LLOG_NORM,
    LLOG_BRIGHT,
    LLOG_OK,
    LLOG_WARN,
    LLOG_ERR,
} LlmLogColor;

typedef struct {
    char         text[LLM_VIS_LOG_COLS];
    LlmLogColor  color;
} LlmLogLine;

typedef struct {
    bool          is_busy;
    char          server[128];
    int           prompt_chars;
    int           response_chars;
    int           bytes_rx;
    char          script_status[72];
    LlmLogColor   script_color;
    LlmLogLine    log[LLM_VIS_LOG_LINES];
    int           log_count;
} LlmVisState;

typedef struct {
    int   match_number;
    int   total_matches;
    float duration;
    int   llm_start;
    int   llm_survivors;
    float llm_avg_hp_frac;
    float damage_dealt;
    int   kills;
    char  winner_name[32];
    char  script_error[512];
} MatchStats;

void llm_bot_init(const char *host, int port, const char *script_path);
void llm_bot_submit_match(const MatchStats *stats);
bool llm_bot_poll_ready(void);
bool llm_bot_poll_gen_error(char *buf, int size);
bool llm_bot_is_busy(void);
void llm_bot_log(LlmLogColor color, const char *fmt, ...);
void llm_bot_get_vis_state(LlmVisState *out);
void llm_bot_shutdown(void);
