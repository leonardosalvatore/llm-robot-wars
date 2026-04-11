#include "llama_client.h"

#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#define HTTP_BUF_SIZE   (512 * 1024)
#define CONNECT_TIMEOUT  10
#define GENERATE_TIMEOUT 300

/* ----------------------------------------------------------------------- */
static int tcp_connect(const char *host, int port, int timeout_sec) {
    struct hostent *he = gethostbyname(host);
    if (!he) {
        printf("[llama] DNS lookup failed for %s\n", host);
        fflush(stdout);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);
    memcpy(&addr.sin_addr, he->h_addr_list[0], (size_t)he->h_length);

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    struct timeval tv = { .tv_sec = timeout_sec, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        printf("[llama] Connection to %s:%d refused\n", host, port);
        fflush(stdout);
        close(sock);
        return -1;
    }
    return sock;
}

static int tcp_recv_all(int sock, int recv_timeout_sec, char *buf, int buf_size) {
    struct timeval tv = { .tv_sec = recv_timeout_sec, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    int total = 0;
    while (total < buf_size - 1) {
        int n = (int)recv(sock, buf + total, (size_t)(buf_size - 1 - total), 0);
        if (n <= 0) break;
        total += n;
    }
    buf[total] = '\0';
    return total;
}

static const char *http_body(const char *resp) {
    const char *p = strstr(resp, "\r\n\r\n");
    if (p) return p + 4;
    p = strstr(resp, "\n\n");
    if (p) return p + 2;
    return resp;
}

/* ----------------------------------------------------------------------- */
static bool json_unescape(const char **src, char *dst, int dst_size) {
    const char *p = *src;
    int di = 0;
    while (*p && di < dst_size - 1) {
        if (*p == '"') { p++; break; }
        if (*p == '\\') {
            p++;
            switch (*p) {
                case '"':  dst[di++] = '"';  break;
                case '\\': dst[di++] = '\\'; break;
                case '/':  dst[di++] = '/';  break;
                case 'n':  dst[di++] = '\n'; break;
                case 'r':  dst[di++] = '\r'; break;
                case 't':  dst[di++] = '\t'; break;
                case 'u':
                    if (p[1] && p[2] && p[3] && p[4]) p += 4;
                    break;
                default:   dst[di++] = *p;   break;
            }
        } else {
            dst[di++] = *p;
        }
        p++;
    }
    dst[di] = '\0';
    *src = p;
    return true;
}

static bool json_get_string(const char *json, const char *key,
                             char *val_buf, int buf_size) {
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(json, needle);
    if (!p) return false;
    p += strlen(needle);
    while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
    if (*p != '"') return false;
    p++;
    return json_unescape(&p, val_buf, buf_size);
}

static void json_escape_str(const char *src, char *dst, int dst_size) {
    int di = 0;
    while (*src && di < dst_size - 3) {
        unsigned char c = (unsigned char)*src++;
        switch (c) {
            case '"':  dst[di++] = '\\'; dst[di++] = '"';  break;
            case '\\': dst[di++] = '\\'; dst[di++] = '\\'; break;
            case '\n': dst[di++] = '\\'; dst[di++] = 'n';  break;
            case '\r': dst[di++] = '\\'; dst[di++] = 'r';  break;
            case '\t': dst[di++] = '\\'; dst[di++] = 't';  break;
            default:
                if (c >= 0x20) dst[di++] = (char)c;
                break;
        }
    }
    dst[di] = '\0';
}

/* ----------------------------------------------------------------------- */

bool llama_server_healthy(const char *host, int port) {
    printf("[llama] Checking server health at %s:%d ...\n", host, port);
    fflush(stdout);

    int sock = tcp_connect(host, port, CONNECT_TIMEOUT);
    if (sock < 0) return false;

    char req[256];
    snprintf(req, sizeof(req),
             "GET /health HTTP/1.0\r\n"
             "Host: %s:%d\r\n"
             "Connection: close\r\n"
             "\r\n",
             host, port);
    send(sock, req, strlen(req), 0);

    char buf[4096];
    tcp_recv_all(sock, CONNECT_TIMEOUT, buf, sizeof(buf));
    close(sock);

    bool ok = (strstr(buf, "200") != NULL);
    printf("[llama] Server health: %s\n", ok ? "OK" : "UNREACHABLE");
    fflush(stdout);
    return ok;
}

int llama_generate(const char *host, int port,
                   const char *prompt,
                   char *response_buf, int buf_size) {
    int prompt_len = (int)strlen(prompt);
    printf("[llama] Sending prompt (%d chars) via /v1/chat/completions ...\n",
           prompt_len);
    fflush(stdout);

    char *esc_prompt = (char *)malloc((size_t)prompt_len * 2 + 16);
    if (!esc_prompt) return -1;
    json_escape_str(prompt, esc_prompt, prompt_len * 2 + 16);

    /*  Use the OpenAI-compatible chat endpoint so reasoning models apply
     *  their chat template and thinking capabilities properly. */
    int body_cap = (int)strlen(esc_prompt) + 512;
    char *json_body = (char *)malloc((size_t)body_cap);
    if (!json_body) { free(esc_prompt); return -1; }
    int body_len = snprintf(json_body, (size_t)body_cap,
        "{\"messages\":[{\"role\":\"user\",\"content\":\"%s\"}],"
        "\"max_tokens\":4096,\"stream\":false}",
        esc_prompt);
    free(esc_prompt);

    printf("[llama] Connecting to %s:%d ...\n", host, port);
    fflush(stdout);

    int sock = tcp_connect(host, port, CONNECT_TIMEOUT);
    if (sock < 0) {
        free(json_body);
        printf("[llama] Failed to connect for generation\n");
        fflush(stdout);
        return -1;
    }

    char header[512];
    snprintf(header, sizeof(header),
             "POST /v1/chat/completions HTTP/1.0\r\n"
             "Host: %s:%d\r\n"
             "Content-Type: application/json\r\n"
             "Content-Length: %d\r\n"
             "Connection: close\r\n"
             "\r\n",
             host, port, body_len);
    send(sock, header, strlen(header), 0);
    send(sock, json_body, (size_t)body_len, 0);
    free(json_body);

    printf("[llama] Waiting for response (timeout %ds) ...\n", GENERATE_TIMEOUT);
    fflush(stdout);

    char *resp = (char *)malloc(HTTP_BUF_SIZE);
    if (!resp) { close(sock); return -1; }

    int bytes = tcp_recv_all(sock, GENERATE_TIMEOUT, resp, HTTP_BUF_SIZE);
    close(sock);
    printf("[llama] Received %d bytes\n", bytes);
    fflush(stdout);

    /* OpenAI chat response: {"choices":[{"message":{"content":"..."}}]} */
    const char *body = http_body(resp);

    /* The "content" key we want is inside "message", which is inside
     * "choices".  There may also be a "reasoning_content" field before it.
     * Find the "message" object first, then extract "content" from there. */
    bool ok = false;
    const char *msg = strstr(body, "\"message\"");
    if (msg) {
        ok = json_get_string(msg, "content", response_buf, buf_size);
    }
    if (!ok) {
        /* Fallback: try top-level "content" (native /completion format) */
        ok = json_get_string(body, "content", response_buf, buf_size);
    }

    if (ok) {
        printf("[llama] Response extracted (%d chars)\n",
               (int)strlen(response_buf));
    } else {
        printf("[llama] Failed to extract response from JSON\n");
        printf("[llama] Body preview: %.300s\n", body);
    }
    fflush(stdout);

    free(resp);
    return ok ? 0 : -1;
}
