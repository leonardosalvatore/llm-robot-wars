#pragma once
#include <stdbool.h>

#define LLAMA_DEFAULT_HOST "127.0.0.1"
#define LLAMA_DEFAULT_PORT  8080

typedef struct {
    char model[128];
    int  prompt_tokens;
    int  completion_tokens;
    int  total_tokens;
} LlamaGenMeta;

/* Check if the llama-server is reachable via GET /health.
 * Returns true if the server responds with 200 OK. */
bool llama_server_healthy(const char *host, int port);

/* Blocking POST /v1/chat/completions with separate system and user messages.
 * On success, response_buf is filled with the unescaped "content" field.
 * Returns 0 on success, -1 on network/parse error. */
int llama_generate(const char *host, int port,
                   const char *system_prompt,
                   const char *user_prompt,
                   char *response_buf, int buf_size,
                   LlamaGenMeta *meta_out);
