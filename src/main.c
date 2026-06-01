#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <math.h>
#include <string.h>

#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"
#define RAYGUI_IMPLEMENTATION
#include "raygui.h"

#include "game.h"
#include "update.h"
#include "scripting.h"
#include "walls.h"
#include "fx.h"
#include "colors.h"
#include "lighting.h"
#include "llama_bot.h"
#include "llama_client.h"

#define COLORS_PATH "colors.cfg"

GameColors g_colors;

/* Global game arrays — declared extern in game.h */
Bot  g_bots[MAX_BOTS];
int  g_bot_count = 0;
Proj g_projs[MAX_PROJECTILES];
int  g_proj_count = 0;

#define CAM_SPEED     8.0f
#define DRAG_PAN_SPEED 0.0006f
#define ORBIT_SPEED   0.0100f
#define ZOOM_SPEED   15.0f
#define ZOOM_MIN      4.0f
#define ZOOM_MAX     60.0f
#define RAD2DEG_F    57.2957795f

#define BAR_W   (CUBE_SIZE * 1.4f)
#define BAR_H   (CUBE_SIZE * 0.12f)
#define BAR_D   (CUBE_SIZE * 0.10f)
#define BAR_Y   (CUBE_SIZE * 2.8f)

/* ----------------------------------------------------------------------- */
static const char *script_paths[TOTAL_SCRIPTS] = {
    "scripts/bot_light.py",
    "scripts/bot_skirmisher.py",
    "scripts/bot_chaser.py",
    "scripts/bot_duelist.py",
    "scripts/bot_lancer.py",
    "scripts/bot_fortress.py",
    "scripts/bot_llm.py",
};

static const char *script_labels[TOTAL_SCRIPTS] = {
    "bot_light", "bot_skirmisher", "bot_chaser",
    "bot_duelist", "bot_lancer", "bot_fortress",
    "bot_llm",
};

/* Team colors now live in g_colors.team[] — loaded from colors.cfg */

/* ----------------------------------------------------------------------- */
typedef struct {
    int   bots_per_type[TOTAL_SCRIPTS];
    float map_width;
    float map_height;
    int   num_walls;
    int   wall_size;
    bool  use_llm;
    bool  reset_llm_bot;
    bool  opposite_corners;
    float bot_increment_per_match;
    char  llm_host[80];
    int   llm_port;
    char  llm_user_prompt[512];
} GameConfig;

static const int   DEFAULT_BOTS[TOTAL_SCRIPTS] = { 2, 1, 1, 1, 1, 1, 5 };
static const float DEFAULT_MAP_WIDTH            = 50.0f;
static const float DEFAULT_MAP_HEIGHT           = 20.0f;
static const int   DEFAULT_NUM_WALLS            = 2;
static const int   BOT_COUNT_MAX                = 200;

static float randf(float lo, float hi) {
    return lo + (float)rand() / (float)RAND_MAX * (hi - lo);
}

#define CFG_PATH "llama-wars.cfg"
#define BOT_LLM_PATH        "scripts/bot_llm.py"
#define BOT_LLM_BACKUP_PATH "scripts/bot_llm.py.backup"

static bool copy_file_overwrite(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (!in) return false;
    FILE *out = fopen(dst, "wb");
    if (!out) { fclose(in); return false; }
    char buf[4096];
    size_t n;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) { ok = false; break; }
    }
    if (ferror(in)) ok = false;
    fclose(in);
    fclose(out);
    return ok;
}

static void reset_llm_bot_script(void) {
    /* Overwrite the working copy used by the engine. Scripts are loaded
       relative to the CWD, so a relative path matches wherever the binary
       is launched from (repo root or build/). */
    copy_file_overwrite(BOT_LLM_BACKUP_PATH, BOT_LLM_PATH);
}

static void cfg_escape_string(const char *src, char *dst, int dst_size) {
    int di = 0;
    while (*src && di < dst_size - 1) {
        char c = *src++;
        if (di >= dst_size - 2) break;
        switch (c) {
            case '\\': dst[di++] = '\\'; dst[di++] = '\\'; break;
            case '\n': dst[di++] = '\\'; dst[di++] = 'n';  break;
            case '\r': dst[di++] = '\\'; dst[di++] = 'r';  break;
            case '\t': dst[di++] = '\\'; dst[di++] = 't';  break;
            default:   dst[di++] = c; break;
        }
    }
    dst[di] = '\0';
}

static void cfg_unescape_string(const char *src, char *dst, int dst_size) {
    int di = 0;
    while (*src && di < dst_size - 1) {
        char c = *src++;
        if (c == '\\' && *src) {
            char n = *src++;
            switch (n) {
                case 'n': dst[di++] = '\n'; break;
                case 'r': dst[di++] = '\r'; break;
                case 't': dst[di++] = '\t'; break;
                case '\\': dst[di++] = '\\'; break;
                default:
                    dst[di++] = n;
                    break;
            }
        } else {
            dst[di++] = c;
        }
    }
    dst[di] = '\0';
}

static void config_set_defaults(GameConfig *cfg) {
    cfg->map_width        = DEFAULT_MAP_WIDTH;
    cfg->map_height       = DEFAULT_MAP_HEIGHT;
    cfg->num_walls        = DEFAULT_NUM_WALLS;
    cfg->wall_size        = 1;
    cfg->use_llm          = false;
    cfg->reset_llm_bot    = true;
    cfg->opposite_corners = true;
    cfg->bot_increment_per_match = 0.0f;
    strncpy(cfg->llm_host, LLAMA_DEFAULT_HOST, sizeof(cfg->llm_host) - 1);
    cfg->llm_port = LLAMA_DEFAULT_PORT;
    cfg->llm_user_prompt[0] = '\0';
    for (int s = 0; s < TOTAL_SCRIPTS; s++)
        cfg->bots_per_type[s] = DEFAULT_BOTS[s];
}

static bool config_load(GameConfig *cfg, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return false;

    char line[2048];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char key[64] = {0};
        char val[1536] = {0};
        if (sscanf(line, " %63[a-z_] = %1535[^\n]", key, val) != 2) continue;
        if (strcmp(key, "llm_user_prompt") != 0) {
            char *comment = strchr(val, '#');
            if (comment) *comment = '\0';
        }
        while (strlen(val) > 0 && val[strlen(val)-1] == ' ') val[strlen(val)-1] = '\0';

        if      (strcmp(key, "map_width")       == 0) cfg->map_width  = (float)atoi(val);
        else if (strcmp(key, "map_height")      == 0) cfg->map_height = (float)atoi(val);
        else if (strcmp(key, "walls")           == 0) cfg->num_walls  = atoi(val);
        else if (strcmp(key, "wall_size")       == 0) cfg->wall_size  = atoi(val);
        else if (strcmp(key, "spawn_mode")      == 0) cfg->opposite_corners = (strstr(val, "corner") != NULL);
        else if (strcmp(key, "bot_increment_per_match") == 0) cfg->bot_increment_per_match = (float)atof(val);
        else if (strcmp(key, "bot_light")       == 0) cfg->bots_per_type[0] = atoi(val);
        else if (strcmp(key, "bot_skirmisher")  == 0) cfg->bots_per_type[1] = atoi(val);
        else if (strcmp(key, "bot_chaser")      == 0) cfg->bots_per_type[2] = atoi(val);
        else if (strcmp(key, "bot_duelist")     == 0) cfg->bots_per_type[3] = atoi(val);
        else if (strcmp(key, "bot_lancer")      == 0) cfg->bots_per_type[4] = atoi(val);
        else if (strcmp(key, "bot_fortress")    == 0) cfg->bots_per_type[5] = atoi(val);
        else if (strcmp(key, "bot_llm")         == 0) cfg->bots_per_type[6] = atoi(val);
        else if (strcmp(key, "llm_host")        == 0) {
            while (*val && val[strlen(val)-1] == ' ') val[strlen(val)-1] = '\0';
            strncpy(cfg->llm_host, val, sizeof(cfg->llm_host) - 1);
        }
        else if (strcmp(key, "llm_port")        == 0) cfg->llm_port = atoi(val);
        else if (strcmp(key, "llm_user_prompt") == 0) {
            char *p = val;
            while (*p == ' ') p++;
            cfg_unescape_string(p, cfg->llm_user_prompt, (int)sizeof(cfg->llm_user_prompt));
        }
    }
    fclose(f);
    return true;
}

static void config_save(const GameConfig *cfg, const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return;
    char escaped_prompt[1536];
    cfg_escape_string(cfg->llm_user_prompt, escaped_prompt, (int)sizeof(escaped_prompt));

    fprintf(f, "map_width        = %-4d # 10-200\n",  (int)cfg->map_width);
    fprintf(f, "map_height       = %-4d # 10-200\n",  (int)cfg->map_height);
    fprintf(f, "walls            = %-4d # 0-40\n",    cfg->num_walls);
    fprintf(f, "wall_size        = %-4d # 1-5 (1=line, 2-5=rectangle)\n", cfg->wall_size);
    fprintf(f, "spawn_mode       = %-8s # corners | random\n",
            cfg->opposite_corners ? "corners" : "random");
    fprintf(f, "bot_increment_per_match = %.2f # percent per match, 1=+1%%, 100=+100%%\n",
            (double)cfg->bot_increment_per_match);
    fprintf(f, "\n");
    fprintf(f, "bot_light        = %-4d # 0-%d\n", cfg->bots_per_type[0], BOT_COUNT_MAX);
    fprintf(f, "bot_skirmisher   = %-4d # 0-%d\n", cfg->bots_per_type[1], BOT_COUNT_MAX);
    fprintf(f, "bot_chaser       = %-4d # 0-%d\n", cfg->bots_per_type[2], BOT_COUNT_MAX);
    fprintf(f, "bot_duelist      = %-4d # 0-%d\n", cfg->bots_per_type[3], BOT_COUNT_MAX);
    fprintf(f, "bot_lancer       = %-4d # 0-%d\n", cfg->bots_per_type[4], BOT_COUNT_MAX);
    fprintf(f, "bot_fortress     = %-4d # 0-%d\n", cfg->bots_per_type[5], BOT_COUNT_MAX);
    fprintf(f, "bot_llm          = %-4d # 0-%d\n", cfg->bots_per_type[6], BOT_COUNT_MAX);
    fprintf(f, "\n");
    fprintf(f, "llm_host         = %s\n", cfg->llm_host);
    fprintf(f, "llm_port         = %-4d # 1-65535\n", cfg->llm_port);
    fprintf(f, "llm_user_prompt  = %s\n", escaped_prompt);

    fclose(f);
}

/* ----------------------------------------------------------------------- */
static void draw_multiline_prompt_box(Rectangle bounds, char *text, int text_size,
                                      bool edit_mode, int max_lines,
                                      bool shift_enter_for_newline) {
    Font font = GetFontDefault();
    const float font_size = 20.0f;
    const float font_spacing = 1.0f;
    Color border = edit_mode ? SKYBLUE : GRAY;
    Color bg     = edit_mode ? (Color){30, 36, 48, 255} : (Color){24, 24, 34, 255};
    DrawRectangleRec(bounds, bg);
    DrawRectangleLinesEx(bounds, 2.0f, border);

    char visible[512];
    int vi = 0;
    int lines = 1;
    {
        for (int i = 0; text[i] != '\0' && vi < (int)sizeof(visible) - 1; i++) {
            visible[vi++] = text[i];
            if (text[i] == '\n') {
                lines++;
                if (lines > max_lines) break;
            }
        }
        visible[vi] = '\0';
        DrawTextEx(font, visible,
                   (Vector2){bounds.x + 8.0f, bounds.y + 8.0f},
                   font_size, font_spacing, RAYWHITE);
    }

    if (edit_mode && ((int)(GetTime() * 2.0) % 2 == 0)) {
        char current_line[512];
        int cli = 0;
        int caret_line = 0;
        for (int i = 0; visible[i] != '\0' && cli < (int)sizeof(current_line) - 1; i++) {
            if (visible[i] == '\n') {
                caret_line++;
                cli = 0;
            } else {
                current_line[cli++] = visible[i];
            }
        }
        current_line[cli] = '\0';

        Vector2 line_size = MeasureTextEx(font, current_line, font_size, font_spacing);
        float line_height = font_size + 4.0f;
        Vector2 caret_pos = {
            bounds.x + 8.0f + line_size.x + 1.0f,
            bounds.y + 8.0f + caret_line * line_height
        };
        DrawTextEx(font, "|", caret_pos, font_size, font_spacing, SKYBLUE);
    }

    if (edit_mode) {
        int len = (int)strlen(text);
        if ((IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) && len > 0) {
            text[len - 1] = '\0';
            len--;
        }
        bool shift_down = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
        if (IsKeyPressed(KEY_ENTER) &&
            (!shift_enter_for_newline || shift_down) &&
            len < text_size - 1) {
            text[len++] = '\n';
            text[len] = '\0';
        }

        int ch = GetCharPressed();
        while (ch > 0) {
            if ((ch >= 32 || ch == '\n') && len < text_size - 1) {
                text[len++] = (char)ch;
                text[len] = '\0';
            }
            ch = GetCharPressed();
        }
    }
}

/* ----------------------------------------------------------------------- */
static void orbit_camera_around_target(Camera3D *camera, float yaw_delta, float pitch_delta) {
    Vector3 offset = Vector3Subtract(camera->position, camera->target);
    float radius = Vector3Length(offset);
    if (radius < 0.001f) radius = 0.001f;

    float yaw   = atan2f(offset.z, offset.x);
    float horiz = sqrtf(offset.x * offset.x + offset.z * offset.z);
    float pitch = atan2f(offset.y, horiz);

    yaw   += yaw_delta;
    pitch += pitch_delta;
    if (pitch >  1.45f) pitch =  1.45f;
    if (pitch < -1.45f) pitch = -1.45f;

    Vector3 next = {
        radius * cosf(pitch) * cosf(yaw),
        radius * sinf(pitch),
        radius * cosf(pitch) * sinf(yaw)
    };
    camera->position = Vector3Add(camera->target, next);
    camera->up = (Vector3){0.0f, 1.0f, 0.0f};
}

static float wall_top_y(void) {
    float top = 0.0f;
    int wn = walls_count();
    const Wall *wv = walls_get();
    for (int i = 0; i < wn; i++) {
        if (wv[i].height > top) top = wv[i].height;
    }
    return top;
}

static void clamp_camera_min_y(Camera3D *camera, float min_y) {
    float lowest = camera->position.y < camera->target.y
                 ? camera->position.y
                 : camera->target.y;
    if (lowest >= min_y) return;

    float lift = min_y - lowest;
    camera->position.y += lift;
    camera->target.y   += lift;
}

static void camera_ground_basis(const Camera3D *camera, Vector3 *forward, Vector3 *right) {
    Vector3 fwd = Vector3Subtract(camera->target, camera->position);
    fwd.y = 0.0f;
    if (Vector3Length(fwd) < 0.001f) {
        fwd = (Vector3){0.0f, 0.0f, -1.0f};
    } else {
        fwd = Vector3Normalize(fwd);
    }

    Vector3 rgt = Vector3CrossProduct(fwd, (Vector3){0.0f, 1.0f, 0.0f});
    if (Vector3Length(rgt) < 0.001f) {
        rgt = (Vector3){1.0f, 0.0f, 0.0f};
    } else {
        rgt = Vector3Normalize(rgt);
    }

    if (forward) *forward = fwd;
    if (right)   *right   = rgt;
}

static float camera_ground_yaw_deg(const Camera3D *camera) {
    Vector3 right;
    camera_ground_basis(camera, NULL, &right);
    return atan2f(-right.z, right.x) * RAD2DEG_F;
}

static Texture2D default_billboard_texture(void) {
    return (Texture2D){
        .id      = rlGetTextureIdDefault(),
        .width   = 1,
        .height  = 1,
        .mipmaps = 1,
        .format  = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8
    };
}

/* ----------------------------------------------------------------------- */
/* spinner_enhance: run AFTER GuiSpinner() with the same bounds. Adds
 *  - mouse wheel over the spinner → ±10
 *  - holding Left / Right arrow for > 0.5 s while hovering → repeat step ±1
 *    (initial keypress already moves by one via the arrow key handling built
 *    into GuiSpinner's edit mode; here we just auto-repeat while held). */
static void spinner_enhance(Rectangle r, int *value, int min_v, int max_v) {
    Vector2 mp = GetMousePosition();
    bool hover = CheckCollisionPointRec(mp, r);
    if (!hover) return;

    int wheel = (int)GetMouseWheelMove();
    if (wheel != 0) {
        int nv = *value + wheel * 10;
        if (nv < min_v) nv = min_v;
        if (nv > max_v) nv = max_v;
        *value = nv;
    }

    static const int MAX_TRACKED = 32;
    static const void *key_ptrs[32];
    static float hold_time[32];
    static float repeat_acc[32];
    static bool  initialised = false;
    if (!initialised) {
        for (int i = 0; i < MAX_TRACKED; i++) {
            key_ptrs[i]   = NULL;
            hold_time[i]  = 0.0f;
            repeat_acc[i] = 0.0f;
        }
        initialised = true;
    }

    int slot = -1;
    for (int i = 0; i < MAX_TRACKED; i++) {
        if (key_ptrs[i] == (const void *)value) { slot = i; break; }
    }
    if (slot < 0) {
        for (int i = 0; i < MAX_TRACKED; i++) {
            if (key_ptrs[i] == NULL) {
                key_ptrs[i] = (const void *)value;
                slot = i;
                break;
            }
        }
    }
    if (slot < 0) return;

    bool left_down  = IsKeyDown(KEY_LEFT);
    bool right_down = IsKeyDown(KEY_RIGHT);
    int dir = 0;
    if (left_down  && !right_down) dir = -1;
    else if (right_down && !left_down) dir = +1;

    if (dir == 0) {
        hold_time[slot]  = 0.0f;
        repeat_acc[slot] = 0.0f;
        return;
    }

    float dt = GetFrameTime();
    hold_time[slot] += dt;
    if (hold_time[slot] < 0.5f) return;

    repeat_acc[slot] += dt;
    const float REPEAT_PERIOD = 0.05f; /* 20 steps/s once repeating kicks in */
    while (repeat_acc[slot] >= REPEAT_PERIOD) {
        repeat_acc[slot] -= REPEAT_PERIOD;
        int nv = *value + dir;
        if (nv < min_v) nv = min_v;
        if (nv > max_v) nv = max_v;
        *value = nv;
    }
}

static bool show_config_screen(GameConfig *cfg) {
    /* Probe llama-server health each time the config screen is shown */
    bool server_available = llama_server_healthy(cfg->llm_host, cfg->llm_port);
    if (server_available && !cfg->use_llm)
        cfg->use_llm = true;

    const int SW      = GetRenderWidth();
    const int SH      = GetRenderHeight();
    const int PW      = 1040;
    const int ROW_H   = 34;
    const int ROWS    = TOTAL_SCRIPTS + 10;
    const int PH      = 60 + ROWS * ROW_H + 16;
    const int PX      = (SW - PW) / 2;
    const int PY      = (SH - PH) / 2 > 10 ? (SH - PH) / 2 : 10;
    const int LBL_W   = 220;
    const int CTL_X   = PX + LBL_W + 10;
    const int CTL_W   = PW - LBL_W - 30;
    const float FONT_SZ = 20.0f;

    bool edit[6 + TOTAL_SCRIPTS];
    for (int i = 0; i < 6 + TOTAL_SCRIPTS; i++) edit[i] = false;
    bool bot_inc_edit = false;

    int map_width_int  = (int)cfg->map_width;
    int map_height_int = (int)cfg->map_height;
    char bot_inc_buf[32];
    snprintf(bot_inc_buf, sizeof(bot_inc_buf), "%.2f", (double)cfg->bot_increment_per_match);

    GuiSetStyle(DEFAULT, TEXT_SIZE, (int)FONT_SZ);

    bool start_with_custom_prompt = false;
    while (!WindowShouldClose()) {
        BeginDrawing();
        ClearBackground(g_colors.bg_config);

        GuiPanel((Rectangle){(float)PX, (float)PY, (float)PW, (float)PH},
                 "Simulation parameters");

        int row = 0;
#define ROW_Y  (PY + 48 + row * ROW_H)

        /* Map width (X axis — the long side for opposite-corners) */
        GuiLabel((Rectangle){(float)(PX + 10), (float)ROW_Y, (float)LBL_W, (float)ROW_H},
                 "Map width (10-200)");
        {
            Rectangle r = {(float)CTL_X, (float)ROW_Y, (float)CTL_W, (float)(ROW_H - 4)};
            if (GuiSpinner(r, NULL, &map_width_int, 10, 200, edit[0]))
                edit[0] = !edit[0];
            spinner_enhance(r, &map_width_int, 10, 200);
            cfg->map_width = (float)map_width_int;
        }
        row++;

        /* Map height (Z axis) */
        GuiLabel((Rectangle){(float)(PX + 10), (float)ROW_Y, (float)LBL_W, (float)ROW_H},
                 "Map height (10-200)");
        {
            Rectangle r = {(float)CTL_X, (float)ROW_Y, (float)CTL_W, (float)(ROW_H - 4)};
            if (GuiSpinner(r, NULL, &map_height_int, 10, 200, edit[1]))
                edit[1] = !edit[1];
            spinner_enhance(r, &map_height_int, 10, 200);
            cfg->map_height = (float)map_height_int;
        }
        row++;

        /* Wall count */
        GuiLabel((Rectangle){(float)(PX + 10), (float)ROW_Y, (float)LBL_W, (float)ROW_H},
                 "Walls (0-40)");
        {
            Rectangle r = {(float)CTL_X, (float)ROW_Y, (float)CTL_W, (float)(ROW_H - 4)};
            if (GuiSpinner(r, NULL, &cfg->num_walls, 0, 40, edit[2]))
                edit[2] = !edit[2];
            spinner_enhance(r, &cfg->num_walls, 0, 40);
        }
        row++;

        /* Wall size */
        GuiLabel((Rectangle){(float)(PX + 10), (float)ROW_Y, (float)LBL_W, (float)ROW_H},
                 "Wall size (1-5)");
        {
            Rectangle r = {(float)CTL_X, (float)ROW_Y, (float)CTL_W, (float)(ROW_H - 4)};
            if (GuiSpinner(r, NULL, &cfg->wall_size, 1, 5, edit[3]))
                edit[3] = !edit[3];
            spinner_enhance(r, &cfg->wall_size, 1, 5);
        }
        row++;

        /* Spawn mode toggle */
        GuiLabel((Rectangle){(float)(PX + 10), (float)ROW_Y, (float)LBL_W, (float)ROW_H},
                 "Spawn mode");
        {
            const char *spawn_text = cfg->opposite_corners
                                     ? "Opposite Corners" : "Random";
            bool opp = cfg->opposite_corners;
            GuiToggle((Rectangle){(float)CTL_X, (float)ROW_Y + 2,
                                  (float)CTL_W, (float)(ROW_H - 6)},
                      spawn_text, &opp);
            cfg->opposite_corners = opp;
        }
        row++;

        /* LLM section */
        {
            bool chk = cfg->use_llm;
            if (!server_available) GuiDisable();
            GuiCheckBox((Rectangle){(float)CTL_X, (float)ROW_Y + 6,
                                    (float)(ROW_H - 12), (float)(ROW_H - 12)},
                        "Use LlaMa AI", &chk);
            cfg->use_llm = chk;
            if (!server_available) {
                GuiEnable();
                GuiLabel((Rectangle){(float)(PX + 10), (float)ROW_Y,
                                     (float)LBL_W, (float)ROW_H},
                         "LLM Server");
                /* Place hint well to the right of the "Use LlaMa AI" label
                   so it doesn't overlap the checkbox text. */
                GuiLabel((Rectangle){(float)(CTL_X + 160), (float)ROW_Y,
                                     300.0f, (float)ROW_H},
                         "(start llama-server first)");
            } else {
                GuiLabel((Rectangle){(float)(PX + 10), (float)ROW_Y,
                                     (float)LBL_W, (float)ROW_H},
                         "LLM Server");
                /* Reset LlaMa bot checkbox, anchored to the right edge of
                   the panel so it doesn't overlap the "Use LlaMa AI" label. */
                bool rst = cfg->reset_llm_bot;
                GuiCheckBox((Rectangle){(float)(PX + PW - 260),
                                        (float)ROW_Y + 6,
                                        (float)(ROW_H - 12),
                                        (float)(ROW_H - 12)},
                            "reset LlaMa bot", &rst);
                cfg->reset_llm_bot = rst;
            }
        }
        row++;

        /* Server endpoint display */
        if (!cfg->use_llm) GuiDisable();
        GuiLabel((Rectangle){(float)(PX + 10), (float)ROW_Y, (float)LBL_W, (float)ROW_H},
                 "Endpoint");
        GuiLabel((Rectangle){(float)CTL_X, (float)ROW_Y, (float)CTL_W, (float)(ROW_H - 4)},
                 TextFormat("%s:%d", cfg->llm_host, cfg->llm_port));
        if (!cfg->use_llm) GuiEnable();
        row++;

        /* Divider */
        GuiLine((Rectangle){(float)(PX + 10), (float)(ROW_Y - 4),
                             (float)(PW - 20), 1}, "Bots per script");
        row++;

        GuiLabel((Rectangle){(float)(PX + 10), (float)ROW_Y,
                             (float)LBL_W, (float)ROW_H},
                 "Per match factor");
        Rectangle bot_inc_minus = {(float)CTL_X, (float)ROW_Y, 70.0f, (float)(ROW_H - 4)};
        Rectangle bot_inc_plus  = {(float)(CTL_X + CTL_W - 70), (float)ROW_Y, 70.0f, (float)(ROW_H - 4)};
        Rectangle bot_inc_rect  = {(float)(CTL_X + 78), (float)ROW_Y,
                                   (float)(CTL_W - 156), (float)(ROW_H - 4)};
        if (GuiButton(bot_inc_minus, "-1%")) {
            cfg->bot_increment_per_match -= 1.0f;
            if (cfg->bot_increment_per_match < 0.0f) cfg->bot_increment_per_match = 0.0f;
            snprintf(bot_inc_buf, sizeof(bot_inc_buf), "%.2f", (double)cfg->bot_increment_per_match);
        }
        if (GuiButton(bot_inc_plus, "+1%")) {
            cfg->bot_increment_per_match += 1.0f;
            snprintf(bot_inc_buf, sizeof(bot_inc_buf), "%.2f", (double)cfg->bot_increment_per_match);
        }
        if (GuiTextBox(bot_inc_rect, bot_inc_buf, (int)sizeof(bot_inc_buf), bot_inc_edit))
            bot_inc_edit = !bot_inc_edit;
        cfg->bot_increment_per_match = (float)atof(bot_inc_buf);
        if (cfg->bot_increment_per_match < 0.0f) cfg->bot_increment_per_match = 0.0f;
        row++;

        /* Bot count spinners */
        for (int s = 0; s < TOTAL_SCRIPTS; s++) {
            GuiLabel((Rectangle){(float)(PX + 10), (float)ROW_Y,
                                 (float)LBL_W, (float)ROW_H},
                     script_labels[s]);
            Rectangle r = {(float)CTL_X, (float)ROW_Y,
                           (float)CTL_W, (float)(ROW_H - 4)};
            if (GuiSpinner(r, NULL, &cfg->bots_per_type[s], 0, BOT_COUNT_MAX, edit[s + 6]))
                edit[s + 6] = !edit[s + 6];
            spinner_enhance(r, &cfg->bots_per_type[s], 0, BOT_COUNT_MAX);

            if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
                if (!CheckCollisionPointRec(GetMousePosition(), r))
                    edit[s + 6] = false;
            }
            row++;
        }

        /* Dismiss spinners on outside click */
        if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
            for (int i = 0; i < 6; i++) {
                int ry = PY + 48 + i * ROW_H;
                Rectangle r = {(float)CTL_X, (float)ry, (float)CTL_W, (float)(ROW_H-4)};
                if (!CheckCollisionPointRec(GetMousePosition(), r))
                    edit[i] = false;
            }
            if (!CheckCollisionPointRec(GetMousePosition(), bot_inc_rect))
                bot_inc_edit = false;
        }

        /* Summary */
        int total = 0;
        for (int s = 0; s < TOTAL_SCRIPTS; s++) total += cfg->bots_per_type[s];
        GuiLabel((Rectangle){(float)(PX + 10), (float)ROW_Y,
                              (float)(PW - 20), (float)ROW_H},
                 TextFormat("Total: %d bots   Map: %d x %d   Walls: %d",
                            total, map_width_int, map_height_int, cfg->num_walls));
        row++;

        /* Start buttons */
        Rectangle start_btn = {(float)(PX + PW / 2 - 360), (float)(ROW_Y + 4),
                               260.0f, (float)(ROW_H - 4)};
        Rectangle prompt_btn = {(float)(PX + PW / 2 + 100), (float)(ROW_Y + 4),
                                260.0f, (float)(ROW_H - 4)};
        if (GuiButton(start_btn, "Start")) {
            EndDrawing();
            break;
        }
        bool prompt_btn_enabled = cfg->use_llm;
        if (!prompt_btn_enabled) GuiDisable();
        if (GuiButton(prompt_btn, "Start with custom prompt")) {
            start_with_custom_prompt = true;
            EndDrawing();
            break;
        }
        if (!prompt_btn_enabled) GuiEnable();

#undef ROW_Y

        EndDrawing();
    }
    return start_with_custom_prompt;
}

/* ----------------------------------------------------------------------- */
/* Draw a tapered wall: base is `taper` fraction wider than the top.       */
static void draw_wall_tapered(float cx, float cy, float cz,
                               float ww, float wh, float wd,
                               float taper, Color col)
{
    float htw = ww * 0.5f;            /* half-top-width  */
    float htd = wd * 0.5f;            /* half-top-depth  */
    float hbw = htw * (1.0f + taper); /* half-bottom-width */
    float hbd = htd * (1.0f + taper); /* half-bottom-depth */
    float yt  = cy + wh * 0.5f;       /* top Y */
    float yb  = cy - wh * 0.5f;       /* bottom Y */

    rlCheckRenderBatchLimit(36);
    rlSetTexture(rlGetTextureIdDefault());
    rlBegin(RL_QUADS);
        rlColor4ub(col.r, col.g, col.b, col.a);

        /* Front face (+Z) */
        float nzf = wh, nyf = (hbd - htd);
        float lenf = sqrtf(nzf*nzf + nyf*nyf);
        if (lenf > 0) { nzf /= lenf; nyf /= lenf; }
        rlNormal3f(0, nyf, nzf);
        rlVertex3f(cx - hbw, yb, cz + hbd);
        rlVertex3f(cx + hbw, yb, cz + hbd);
        rlVertex3f(cx + htw, yt, cz + htd);
        rlVertex3f(cx - htw, yt, cz + htd);

        /* Back face (-Z) */
        rlNormal3f(0, nyf, -nzf);
        rlVertex3f(cx + hbw, yb, cz - hbd);
        rlVertex3f(cx - hbw, yb, cz - hbd);
        rlVertex3f(cx - htw, yt, cz - htd);
        rlVertex3f(cx + htw, yt, cz - htd);

        /* Right face (+X) */
        float nxr = wh, nyr = (hbw - htw);
        float lenr = sqrtf(nxr*nxr + nyr*nyr);
        if (lenr > 0) { nxr /= lenr; nyr /= lenr; }
        rlNormal3f(nxr, nyr, 0);
        rlVertex3f(cx + hbw, yb, cz + hbd);
        rlVertex3f(cx + hbw, yb, cz - hbd);
        rlVertex3f(cx + htw, yt, cz - htd);
        rlVertex3f(cx + htw, yt, cz + htd);

        /* Left face (-X) */
        rlNormal3f(-nxr, nyr, 0);
        rlVertex3f(cx - hbw, yb, cz - hbd);
        rlVertex3f(cx - hbw, yb, cz + hbd);
        rlVertex3f(cx - htw, yt, cz + htd);
        rlVertex3f(cx - htw, yt, cz - htd);

        /* Top face */
        rlNormal3f(0, 1, 0);
        rlVertex3f(cx - htw, yt, cz - htd);
        rlVertex3f(cx - htw, yt, cz + htd);
        rlVertex3f(cx + htw, yt, cz + htd);
        rlVertex3f(cx + htw, yt, cz - htd);

        /* Bottom face */
        rlNormal3f(0, -1, 0);
        rlVertex3f(cx - hbw, yb, cz + hbd);
        rlVertex3f(cx - hbw, yb, cz - hbd);
        rlVertex3f(cx + hbw, yb, cz - hbd);
        rlVertex3f(cx + hbw, yb, cz + hbd);

    rlEnd();
    rlSetTexture(0);
}

/* Wire-frame outline for a tapered wall. */
static void draw_wall_tapered_wires(float cx, float cy, float cz,
                                     float ww, float wh, float wd,
                                     float taper, Color col)
{
    float htw = ww * 0.5f;
    float htd = wd * 0.5f;
    float hbw = htw * (1.0f + taper);
    float hbd = htd * (1.0f + taper);
    float yt  = cy + wh * 0.5f;
    float yb  = cy - wh * 0.5f;

    /* Expand all corners by a tiny epsilon so the wire sits fractionally
     * in front of the solid fill, eliminating Z-fighting. */
    const float e = 0.012f;
    htw += e;  htd += e;
    hbw += e;  hbd += e;
    yt  += e;  yb  -= e;

    /* 8 corners: t=top, b=bottom; order: front-left, front-right, back-right, back-left */
    Vector3 tf = {cx - htw, yt, cz + htd};
    Vector3 tr = {cx + htw, yt, cz + htd};
    Vector3 tbr= {cx + htw, yt, cz - htd};
    Vector3 tbl= {cx - htw, yt, cz - htd};
    Vector3 bf = {cx - hbw, yb, cz + hbd};
    Vector3 brr= {cx + hbw, yb, cz + hbd};
    Vector3 bbr= {cx + hbw, yb, cz - hbd};
    Vector3 bbl= {cx - hbw, yb, cz - hbd};

    /* Top ring */
    DrawLine3D(tf,  tr,  col);
    DrawLine3D(tr,  tbr, col);
    DrawLine3D(tbr, tbl, col);
    DrawLine3D(tbl, tf,  col);
    /* Bottom ring */
    DrawLine3D(bf,  brr, col);
    DrawLine3D(brr, bbr, col);
    DrawLine3D(bbr, bbl, col);
    DrawLine3D(bbl, bf,  col);
    /* Vertical edges */
    DrawLine3D(tf,  bf,  col);
    DrawLine3D(tr,  brr, col);
    DrawLine3D(tbr, bbr, col);
    DrawLine3D(tbl, bbl, col);
}

/* ----------------------------------------------------------------------- */
/* Bounding-box dimensions used for placement on the chassis. The actual model
 * drawn by draw_weapon_at() is built from multiple primitives within this box,
 * with the receiver behind the mount point and the barrel extending forward. */
static void weapon_dims(WeaponType wt, float *ww, float *wh, float *wd) {
    if (wt == WEAPON_MACHINE_GUN) {
        *ww = CUBE_SIZE * 0.32f; *wh = CUBE_SIZE * 0.32f; *wd = CUBE_SIZE * 0.95f;
    } else if (wt == WEAPON_AUTO_CANNON) {
        *ww = CUBE_SIZE * 0.50f; *wh = CUBE_SIZE * 0.50f; *wd = CUBE_SIZE * 1.20f;
    } else {
        *ww = CUBE_SIZE * 0.34f; *wh = CUBE_SIZE * 0.34f; *wd = CUBE_SIZE * 1.10f;
    }
}

static Color color_scale(Color c, float k) {
    int r = (int)(c.r * k); if (r > 255) r = 255; if (r < 0) r = 0;
    int g = (int)(c.g * k); if (g > 255) g = 255; if (g < 0) g = 0;
    int b = (int)(c.b * k); if (b > 255) b = 255; if (b < 0) b = 0;
    return (Color){ (unsigned char)r, (unsigned char)g, (unsigned char)b, c.a };
}

/* Draw a weapon model centred at (cx, cy, cz) in the current local frame.
 * Barrel points along local +z (forward). Each weapon is built from a few
 * primitives so the silhouette is recognisable at gameplay zoom. */
static void draw_weapon_at(WeaponType wt, float cx, float cy, float cz, Color col) {
    float ww, wh, wd;
    weapon_dims(wt, &ww, &wh, &wd);

    Color dark   = color_scale(col, 0.55f);
    Color darker = color_scale(col, 0.35f);
    Color hot    = (Color){ 255, 210,  80, 255 };

    if (wt == WEAPON_MACHINE_GUN) {
        /* Boxy receiver, side ammo can, ribbed barrel, muzzle brake. */
        float rec_d = wd * 0.40f;
        Vector3 rec_c = { cx, cy, cz - wd * 0.30f };
        DrawCube(rec_c, ww, wh, rec_d, col);
        DrawCubeWires(rec_c, ww, wh, rec_d, BLACK);

        float amo_w = ww * 0.50f, amo_h = wh * 0.70f, amo_d = rec_d * 0.85f;
        Vector3 amo_c = { cx + ww * 0.5f + amo_w * 0.5f + 0.005f,
                          cy - wh * 0.10f, cz - wd * 0.30f };
        DrawCube(amo_c, amo_w, amo_h, amo_d, dark);
        DrawCubeWires(amo_c, amo_w, amo_h, amo_d, BLACK);

        float bar_r = ww * 0.18f;
        Vector3 ba = { cx, cy, cz - wd * 0.10f };
        Vector3 bb = { cx, cy, cz + wd * 0.48f };
        DrawCylinderEx(ba, bb, bar_r, bar_r * 0.85f, 10, dark);
        DrawCylinderWiresEx(ba, bb, bar_r, bar_r * 0.85f, 10, BLACK);

        Vector3 ma = { cx, cy, cz + wd * 0.48f };
        Vector3 mb = { cx, cy, cz + wd * 0.60f };
        DrawCylinderEx(ma, mb, bar_r * 1.7f, bar_r * 1.4f, 10, col);
        DrawCylinderWiresEx(ma, mb, bar_r * 1.7f, bar_r * 1.4f, 10, BLACK);
    }
    else if (wt == WEAPON_AUTO_CANNON) {
        /* Chunky breech with a top recoil cradle and a fat long barrel. */
        float bre_d = wd * 0.42f;
        Vector3 bre_c = { cx, cy, cz - wd * 0.29f };
        DrawCube(bre_c, ww, wh, bre_d, col);
        DrawCubeWires(bre_c, ww, wh, bre_d, BLACK);

        float cra_w = ww * 0.70f, cra_h = wh * 0.30f, cra_d = bre_d * 0.80f;
        Vector3 cra_c = { cx, cy + wh * 0.5f + cra_h * 0.5f, cz - wd * 0.29f };
        DrawCube(cra_c, cra_w, cra_h, cra_d, dark);
        DrawCubeWires(cra_c, cra_w, cra_h, cra_d, BLACK);

        float bar_r = ww * 0.30f;
        Vector3 ba = { cx, cy, cz - wd * 0.08f };
        Vector3 bb = { cx, cy, cz + wd * 0.48f };
        DrawCylinderEx(ba, bb, bar_r, bar_r, 12, dark);
        DrawCylinderWiresEx(ba, bb, bar_r, bar_r, 12, BLACK);

        /* Recoil ring near the breech end of the barrel. */
        Vector3 ra = { cx, cy, cz - wd * 0.04f };
        Vector3 rb = { cx, cy, cz + wd * 0.04f };
        DrawCylinderEx(ra, rb, bar_r * 1.35f, bar_r * 1.35f, 12, darker);
        DrawCylinderWiresEx(ra, rb, bar_r * 1.35f, bar_r * 1.35f, 12, BLACK);

        /* Muzzle ring (slightly flared). */
        Vector3 ma = { cx, cy, cz + wd * 0.48f };
        Vector3 mb = { cx, cy, cz + wd * 0.60f };
        DrawCylinderEx(ma, mb, bar_r * 1.45f, bar_r * 1.25f, 12, col);
        DrawCylinderWiresEx(ma, mb, bar_r * 1.45f, bar_r * 1.25f, 12, BLACK);
    }
    else {
        /* WEAPON_LASER: emitter housing with cooling fins + slim focused tube
         * + hot lens disc at the muzzle. */
        float emi_d = wd * 0.42f;
        Vector3 emi_c = { cx, cy, cz - wd * 0.29f };
        DrawCube(emi_c, ww, wh, emi_d, col);
        DrawCubeWires(emi_c, ww, wh, emi_d, BLACK);

        float fin_w = ww * 0.85f, fin_h = wh * 0.20f, fin_d = emi_d * 0.16f;
        for (int i = 0; i < 3; i++) {
            float fz = (i - 1) * (emi_d * 0.32f);
            Vector3 fc = { cx, cy + wh * 0.5f + fin_h * 0.5f,
                           cz - wd * 0.29f + fz };
            DrawCube(fc, fin_w, fin_h, fin_d, darker);
            DrawCubeWires(fc, fin_w, fin_h, fin_d, BLACK);
        }

        float tub_r = ww * 0.16f;
        Vector3 ta = { cx, cy, cz - wd * 0.08f };
        Vector3 tb = { cx, cy, cz + wd * 0.46f };
        DrawCylinderEx(ta, tb, tub_r, tub_r * 0.6f, 12, dark);
        DrawCylinderWiresEx(ta, tb, tub_r, tub_r * 0.6f, 12, BLACK);

        Vector3 la = { cx, cy, cz + wd * 0.46f };
        Vector3 lb = { cx, cy, cz + wd * 0.54f };
        DrawCylinderEx(la, lb, tub_r * 1.8f, tub_r * 1.8f, 12, hot);
        DrawCylinderWiresEx(la, lb, tub_r * 1.8f, tub_r * 1.8f, 12, BLACK);
    }
}

/* Returns the locomotion's vertical "deck" height at which the body sits. */
static float locomotion_deck_h(Locomotion l) {
    switch (l) {
        case LOCO_TRACKS: return CUBE_SIZE * 0.5f;
        case LOCO_WHEELS: return CUBE_SIZE * 0.5f;
        case LOCO_LEGS_4: return CUBE_SIZE * 0.7f;
        case LOCO_LEGS_2: return CUBE_SIZE * 0.7f;
        default:          return CUBE_SIZE * 0.5f;
    }
}

/* Top-of-body world Y, used for HP/armour bar placement. */
static float bot_body_top_y(const BotConfig *cfg) {
    return locomotion_deck_h(cfg->locomotion) + CUBE_SIZE * cfg->body_sy;
}

static void draw_locomotion(const BotConfig *cfg, const BotInertia *iner) {
    Color trk_col = g_colors.bot_tread;
    Color trk_dim = color_scale(trk_col, 0.55f);
    Color trk_drk = color_scale(trk_col, 0.35f);
    float bs   = CUBE_SIZE;
    float bx   = bs * cfg->body_sx;
    float bz   = bs * cfg->body_sz;
    float deck = locomotion_deck_h(cfg->locomotion);

    switch (cfg->locomotion) {
        case LOCO_TRACKS: {
            /* Two side plates with a drive sprocket + idler wheel + a row of
             * exposed track shoes along the bottom of each side. */
            float trk_sx   = bx * 1.05f;
            float trk_sz   = bz * 1.25f;
            float trk_h    = deck;
            float plate_w  = bs * 0.20f;
            float plate_offset = (trk_sx - plate_w) * 0.5f;

            for (int side = -1; side <= 1; side += 2) {
                float px = (float)side * plate_offset;

                Vector3 pc = { px, trk_h * 0.55f, 0.0f };
                DrawCube(pc, plate_w, trk_h * 0.85f, trk_sz * 0.85f, trk_col);
                DrawCubeWires(pc, plate_w, trk_h * 0.85f, trk_sz * 0.85f, BLACK);

                float spr_r = trk_h * 0.50f;
                float spr_w = plate_w * 1.20f;
                for (int end = -1; end <= 1; end += 2) {
                    float ez = (float)end * (trk_sz * 0.5f - spr_r);
                    Vector3 sa = { px - spr_w * 0.5f, spr_r, ez };
                    Vector3 sb = { px + spr_w * 0.5f, spr_r, ez };
                    DrawCylinderEx(sa, sb, spr_r, spr_r, 12, trk_drk);
                    DrawCylinderWiresEx(sa, sb, spr_r, spr_r, 12, BLACK);
                }

                /* Track shoes (small dark cubes) along the bottom. */
                int   shoe_n  = 6;
                float shoe_w  = plate_w * 1.30f;
                float shoe_h  = bs * 0.08f;
                float shoe_d  = trk_sz * 0.14f;
                float shoe_span = trk_sz - 2.0f * spr_r;
                float shoe_gap  = shoe_span / (float)shoe_n;
                for (int k = 0; k < shoe_n; k++) {
                    float sz = -shoe_span * 0.5f + (k + 0.5f) * shoe_gap;
                    Vector3 c = { px, shoe_h * 0.55f, sz };
                    DrawCube(c, shoe_w, shoe_h, shoe_d * 0.85f, trk_drk);
                    DrawCubeWires(c, shoe_w, shoe_h, shoe_d * 0.85f, BLACK);
                }
            }
            break;
        }
        case LOCO_WHEELS: {
            /* Bigger, wider tyres with a darker exposed hub on the outside. */
            float wh_r  = deck * 0.62f;
            float wh_w  = bs * 0.26f;
            float wh_y  = wh_r;
            float wxoff = bx * 0.55f + wh_w * 0.20f;
            float wzoff = bz * 0.42f;
            Vector3 wheels[4] = {
                { -wxoff, wh_y, +wzoff },
                { +wxoff, wh_y, +wzoff },
                { -wxoff, wh_y, -wzoff },
                { +wxoff, wh_y, -wzoff },
            };
            for (int i = 0; i < 4; i++) {
                Vector3 a = { wheels[i].x - wh_w * 0.5f, wheels[i].y, wheels[i].z };
                Vector3 b = { wheels[i].x + wh_w * 0.5f, wheels[i].y, wheels[i].z };
                DrawCylinderEx(a, b, wh_r, wh_r, 14, trk_col);
                DrawCylinderWiresEx(a, b, wh_r, wh_r, 14, BLACK);

                /* Hub: smaller darker cylinder that sticks slightly out the
                 * outboard side. */
                float hub_r = wh_r * 0.40f;
                float outboard = (wheels[i].x > 0.0f) ? +1.0f : -1.0f;
                Vector3 ha = { wheels[i].x + outboard * wh_w * 0.35f,
                               wheels[i].y, wheels[i].z };
                Vector3 hb = { wheels[i].x + outboard * wh_w * 0.55f,
                               wheels[i].y, wheels[i].z };
                DrawCylinderEx(ha, hb, hub_r, hub_r, 12, trk_drk);
                DrawCylinderWiresEx(ha, hb, hub_r, hub_r, 12, BLACK);
            }
            break;
        }
        case LOCO_LEGS_4:
        case LOCO_LEGS_2: {
            /* Two-segment legs (thigh + shin) with a darker knee bulge. The
             * hip swings forward/back and the knee bends counter to keep the
             * foot tracking roughly under the hip — a cheap but readable gait. */
            int   n        = (cfg->locomotion == LOCO_LEGS_4) ? 4 : 2;
            float total_h  = deck;
            float thigh_h  = total_h * 0.55f;
            float shin_h   = total_h * 0.45f;
            float thigh_w  = bs * 0.26f;
            float thigh_d  = bs * 0.22f;
            float shin_w   = bs * 0.20f;
            float shin_d   = bs * 0.18f;
            float knee_w   = bs * 0.28f;
            float swing    = sinf(iner->move_anim_t * 6.0f) * 0.35f;

            for (int i = 0; i < n; i++) {
                float lx, lz, my_swing;
                if (n == 4) {
                    bool front = (i < 2);
                    bool right = ((i % 2) == 1);
                    lx = right ? +bx * 0.45f : -bx * 0.45f;
                    lz = front ? +bz * 0.45f : -bz * 0.45f;
                    my_swing = (front == right) ? swing : -swing;
                } else {
                    lx = (i == 0) ? -bx * 0.45f : +bx * 0.45f;
                    lz = 0.0f;
                    my_swing = (i == 0) ? swing : -swing;
                }
                rlPushMatrix();
                    /* Hip pivot at deck height. */
                    rlTranslatef(lx, total_h, lz);
                    rlRotatef(my_swing * RAD2DEG_F, 1.0f, 0.0f, 0.0f);

                    /* Thigh hangs down from hip. */
                    DrawCube((Vector3){0, -thigh_h * 0.5f, 0},
                             thigh_w, thigh_h, thigh_d, trk_col);
                    DrawCubeWires((Vector3){0, -thigh_h * 0.5f, 0},
                                  thigh_w, thigh_h, thigh_d, BLACK);

                    /* Knee joint: small darker chunky cube at the bend. */
                    rlTranslatef(0.0f, -thigh_h, 0.0f);
                    DrawCube((Vector3){0, 0, 0},
                             knee_w, knee_w * 0.55f, thigh_d * 1.05f, trk_drk);
                    DrawCubeWires((Vector3){0, 0, 0},
                                  knee_w, knee_w * 0.55f, thigh_d * 1.05f, BLACK);

                    /* Shin counter-bends to keep the foot roughly under the
                     * body even as the hip swings. */
                    rlRotatef(-my_swing * RAD2DEG_F * 1.1f, 1.0f, 0.0f, 0.0f);
                    DrawCube((Vector3){0, -shin_h * 0.5f, 0},
                             shin_w, shin_h, shin_d, trk_dim);
                    DrawCubeWires((Vector3){0, -shin_h * 0.5f, 0},
                                  shin_w, shin_h, shin_d, BLACK);
                rlPopMatrix();
            }
            break;
        }
    }
}

static void draw_bot(float cx, float cz, Color color,
                     const BotConfig *cfg, const BotInertia *iner)
{
    float bs   = CUBE_SIZE;
    float bx   = bs * cfg->body_sx;
    float by   = bs * cfg->body_sy;
    float bz   = bs * cfg->body_sz;
    float deck = locomotion_deck_h(cfg->locomotion);
    float bod_y = deck + by * 0.5f;

    float body_deg = 90.0f - iner->body_angle * RAD2DEG_F;
    rlPushMatrix();
        rlTranslatef(cx, 0.0f, cz);
        rlRotatef(body_deg, 0.0f, 1.0f, 0.0f);

        draw_locomotion(cfg, iner);

        DrawCube((Vector3){0, bod_y, 0}, bx, by, bz, color);
        DrawCubeWires((Vector3){0, bod_y, 0}, bx, by, bz, BLACK);
    rlPopMatrix();

    /* Turret (carries weapons; rotates independently of body). */
    float turret_deg = 90.0f - iner->turret_angle * RAD2DEG_F;
    Color wcolor = g_colors.bot_weapon;

    rlPushMatrix();
        rlTranslatef(cx, bod_y, cz);
        rlRotatef(turret_deg, 0.0f, 1.0f, 0.0f);

        for (int i = 0; i < cfg->weapon_count; i++) {
            const WeaponSlot *ws = &cfg->weapons[i];
            float ww, wh, wd;
            weapon_dims(ws->type, &ww, &wh, &wd);

            float half_x = bx * 0.5f;
            float half_z = bz * 0.5f;
            float top_y  = by * 0.5f;
            float mx = 0.0f, my = 0.0f, mz = 0.0f;

            switch (ws->mount) {
                case MOUNT_LEFT:
                    mx = -(half_x + ww * 0.5f + 0.02f); my = 0.0f; mz = 0.0f;
                    break;
                case MOUNT_RIGHT:
                    mx = +(half_x + ww * 0.5f + 0.02f); my = 0.0f; mz = 0.0f;
                    break;
                case MOUNT_TOP:
                    mx = 0.0f;                          my = top_y + wh * 0.6f; mz = 0.0f;
                    break;
                case MOUNT_TOP_FRONT:
                    mx = 0.0f;                          my = top_y + wh * 0.6f; mz = +half_z * 0.6f;
                    break;
                case MOUNT_TOP_REAR:
                    mx = 0.0f;                          my = top_y + wh * 0.6f; mz = -half_z * 0.6f;
                    break;
            }
            draw_weapon_at(ws->type, mx, my, mz, wcolor);
        }
    rlPopMatrix();
}

/* ----------------------------------------------------------------------- */
typedef struct {
    int  spawn_count[TOTAL_SCRIPTS];
    char llm_load_error[512];
} MatchState;

static void respawn_team(int script_idx, const GameConfig *gcfg,
                         float arena_half_x, float arena_half_z)
{
    for (int i = 0; i < g_bot_count; i++) {
        Bot *bot = &g_bots[i];
        if (bot->script_id != script_idx) continue;
        if (bot->active) continue;

        float x, z;
        int tries = 0;
        do {
            if (gcfg->opposite_corners) {
                if (script_idx == LLM_SCRIPT_IDX)
                    x = randf(arena_half_x * 0.5f, arena_half_x - 0.5f);
                else
                    x = randf(-arena_half_x + 0.5f, -arena_half_x * 0.5f);
                z = randf(-arena_half_z + 0.5f, arena_half_z - 0.5f);
            } else {
                x = randf(-arena_half_x + 0.5f, arena_half_x - 0.5f);
                z = randf(-arena_half_z + 0.5f, arena_half_z - 0.5f);
            }
            tries++;
        } while (!walls_safe_spawn(x, z, 1.0f) && tries < 200);

        PyObject *ns = scripting_load(script_paths[script_idx]);
        BotConfig cfg;
        if (ns) {
            scripting_call_init(ns, &cfg);
        } else {
            memset(&cfg, 0, sizeof(cfg));
            cfg.locomotion       = LOCO_WHEELS;
            cfg.body             = BODY_CUBE;
            cfg.weapon_count     = 2;
            cfg.weapons[0].type  = WEAPON_AUTO_CANNON;
            cfg.weapons[0].mount = MOUNT_LEFT;
            cfg.weapons[1].type  = WEAPON_AUTO_CANNON;
            cfg.weapons[1].mount = MOUNT_RIGHT;
            cfg.max_hp     = 150.0f;
            cfg.max_speed  = 3.5f;
            cfg.turn_rate  = 7.0f;
            cfg.body_sx    = 1.0f;
            cfg.body_sy    = 1.0f;
            cfg.body_sz    = 1.0f;
            cfg.total_weight = 2.8f;
        }
        cfg.script_idx = script_idx;

        Color col = g_colors.team[script_idx];
        bot->active    = true;
        bot->x         = x;
        bot->y         = 0.0f;
        bot->z         = z;
        bot->vx        = 0.0f;
        bot->vy        = 0.0f;
        bot->vz        = 0.0f;
        bot->r         = col.r;
        bot->g         = col.g;
        bot->b         = col.b;
        bot->a         = col.a;
        bot->hp        = cfg.max_hp;
        bot->config    = cfg;
        bot->inertia   = (BotInertia){0};
        bot->py_ns     = ns;
    }
}

static void kill_team(int script_idx) {
    for (int i = 0; i < g_bot_count; i++) {
        Bot *bot = &g_bots[i];
        if (bot->script_id != script_idx) continue;
        bot->active = false;
        bot->hp     = 0.0f;
    }
}

static void match_setup(MatchState *ms, const GameConfig *gcfg,
                        float arena_half_x, float arena_half_z,
                        unsigned wall_seed, int match_idx)
{
    g_bot_count  = 0;
    g_proj_count = 0;
    memset(g_bots,  0, sizeof(g_bots));
    memset(g_projs, 0, sizeof(g_projs));

    update_set_arena(arena_half_x, arena_half_z);
    walls_generate(arena_half_x, arena_half_z, gcfg->num_walls, gcfg->wall_size, wall_seed);
    walls_add_border(arena_half_x, arena_half_z);
    scripting_init();

    ms->llm_load_error[0] = '\0';
    for (int s = 0; s < TOTAL_SCRIPTS; s++) {
        float growth = (s == LLM_SCRIPT_IDX)
                     ? 1.0f
                     : 1.0f + (gcfg->bot_increment_per_match * 0.01f) * (float)match_idx;
        if (growth < 0.0f) growth = 0.0f;
        int n = (int)lroundf((double)gcfg->bots_per_type[s] * (double)growth);
        ms->spawn_count[s] = n;
        Color col = g_colors.team[s];

        for (int b = 0; b < n; b++) {
            if (g_bot_count >= MAX_BOTS) break;

            float x, z;
            int tries = 0;
            do {
                if (gcfg->opposite_corners) {
                    if (s == LLM_SCRIPT_IDX) {
                        x = randf(arena_half_x * 0.5f, arena_half_x - 0.5f);
                    } else {
                        x = randf(-arena_half_x + 0.5f, -arena_half_x * 0.5f);
                    }
                    z = randf(-arena_half_z + 0.5f, arena_half_z - 0.5f);
                } else {
                    x = randf(-arena_half_x + 0.5f, arena_half_x - 0.5f);
                    z = randf(-arena_half_z + 0.5f, arena_half_z - 0.5f);
                }
                tries++;
            } while (!walls_safe_spawn(x, z, 1.0f) && tries < 200);

            PyObject *ns = scripting_load(script_paths[s]);
            if (!ns && s == LLM_SCRIPT_IDX && ms->llm_load_error[0] == '\0') {
                const char *err = scripting_get_last_error();
                if (err)
                    snprintf(ms->llm_load_error, sizeof(ms->llm_load_error), "%s", err);
            }
            BotConfig cfg;
            if (ns) {
                scripting_call_init(ns, &cfg);
            } else {
                memset(&cfg, 0, sizeof(cfg));
                cfg.locomotion       = LOCO_WHEELS;
                cfg.body             = BODY_CUBE;
                cfg.weapon_count     = 2;
                cfg.weapons[0].type  = WEAPON_AUTO_CANNON;
                cfg.weapons[0].mount = MOUNT_LEFT;
                cfg.weapons[1].type  = WEAPON_AUTO_CANNON;
                cfg.weapons[1].mount = MOUNT_RIGHT;
                cfg.max_hp     = 150.0f;
                cfg.max_speed  = 3.5f;
                cfg.turn_rate  = 7.0f;
                cfg.body_sx    = 1.0f;
                cfg.body_sy    = 1.0f;
                cfg.body_sz    = 1.0f;
                cfg.total_weight = 2.8f;
            }
            cfg.script_idx = s;

            int idx = g_bot_count++;
            Bot *bot       = &g_bots[idx];
            bot->active    = true;
            bot->x         = x;
            bot->y         = 0.0f;
            bot->z         = z;
            bot->vx        = 0.0f;
            bot->vy        = 0.0f;
            bot->vz        = 0.0f;
            bot->r         = col.r;
            bot->g         = col.g;
            bot->b         = col.b;
            bot->a         = col.a;
            bot->hp        = cfg.max_hp;
            bot->script_id = s;
            bot->config    = cfg;
            bot->inertia   = (BotInertia){0};
            bot->py_ns     = ns;
        }
    }
}

static void match_teardown(void) {
    scripting_shutdown();
    g_bot_count  = 0;
    g_proj_count = 0;
}

/* ----------------------------------------------------------------------- */
static void draw_llm_panel(const LlmVisState *vis, int generation)
{
    const int PW  = 400;
    const int FSZ = 15;
    const int LH  = FSZ + 5;
    const int PAD = 8;

    int SW = GetRenderWidth();
    int SH = GetRenderHeight();
    int PX = SW - PW;
    int PY = 0;
    int PH = SH;

    Color bg       = {0,  10,  0, 210};
    Color scanline = {0,   0,  0,  55};
    Color border   = {0, 140,  0, 200};
    Color dim      = {0,  90,  0, 255};
    Color norm     = {0, 175,  0, 255};
    Color bright   = {0, 255, 60, 255};
    Color ok_col   = {80, 255, 140, 255};
    Color warn_col = {220, 200,  0, 255};
    Color err_col  = {230,  50, 50, 255};

    Color log_colors[] = { dim, norm, bright, ok_col, warn_col, err_col };

    DrawRectangle(PX, PY, PW, PH, bg);
    for (int y = PY; y < PY + PH; y += 3)
        DrawRectangle(PX, y, PW, 1, scanline);
    DrawRectangle(PX, PY, 2, PH, border);

    int cx = PX + PAD;
    int cy = PY + 6;

    DrawText("LLAMA NEURAL LINK", cx, cy, FSZ + 3, bright);
    cy += LH + 4;

    for (int x = PX + 2; x < PX + PW; x += 6)
        DrawText("=", x, cy, FSZ - 2, dim);
    cy += LH - 4;

    char srv[60];
    snprintf(srv, sizeof(srv), "%.55s", vis->server[0] ? vis->server : "none");
    DrawText("SRV>", cx, cy, FSZ, dim);
    DrawText(srv,    cx + 46, cy, FSZ, norm);
    cy += LH;

    bool busy   = vis->is_busy;
    int  spin   = (int)(GetTime() * 10.0) % 4;
    const char *spinners[] = {"|", "/", "-", "\\"};
    const char *sts_txt;
    Color sts_col;
    if (busy) {
        sts_txt = TextFormat("[%s GENERATING...]", spinners[spin]);
        sts_col = ((int)(GetTime() * 2) % 2) ? bright : ok_col;
    } else {
        sts_txt = "[  IDLE         ]";
        sts_col = dim;
    }
    DrawText("STS>", cx,       cy, FSZ, dim);
    DrawText(sts_txt, cx + 46, cy, FSZ, sts_col);
    cy += LH;

    DrawText("GEN>", cx,      cy, FSZ, dim);
    DrawText(TextFormat("%d", generation),
             cx + 46, cy, FSZ, norm);
    cy += LH;

    for (int x = PX + 2; x < PX + PW - 2; x += 6)
        DrawText("-", x, cy, FSZ - 2, dim);
    cy += LH - 4;

    DrawText("LAST EXCHANGE", cx, cy, FSZ, dim);
    cy += LH;

    DrawText(" >>", cx, cy, FSZ, dim);
    DrawText(TextFormat("prompt   %5d chars", vis->prompt_chars),
             cx + 30, cy, FSZ, norm);
    cy += LH;

    DrawText(" <<", cx, cy, FSZ, dim);
    DrawText(TextFormat("response %5d chars", vis->response_chars),
             cx + 30, cy, FSZ, norm);
    cy += LH;

    Color sc = log_colors[vis->script_color < 6 ? vis->script_color : 0];
    DrawText(" SCR", cx, cy, FSZ, dim);
    DrawText(TextFormat("  [%s]", vis->script_status), cx + 30, cy, FSZ, sc);
    cy += LH;

    for (int x = PX + 2; x < PX + PW - 2; x += 6)
        DrawText("-", x, cy, FSZ - 2, dim);
    cy += LH - 4;

    DrawText("ACTIVITY LOG", cx, cy, FSZ, dim);
    cy += LH;

    int rows_available = (PY + PH - cy - LH) / LH;
    if (rows_available < 1) rows_available = 1;

    int start = vis->log_count - rows_available;
    if (start < 0) start = 0;
    for (int i = start; i < vis->log_count; i++) {
        const LlmLogLine *ll = &vis->log[i];
        Color lc = log_colors[ll->color < 6 ? ll->color : 0];
        DrawText(ll->text, cx, cy, FSZ, lc);
        cy += LH;
    }

    if ((int)(GetTime() * 2.0) % 2 == 0)
        DrawText("_", cx, cy, FSZ, bright);
}

/* ----------------------------------------------------------------------- */
/* Snapshot the current LLM generation's telemetry and submit it to the LLM
 * pipeline. Called at each deploy boundary (LLM team wiped or forced redeploy)
 * so the next generation learns from how the previous one actually fought. */
static void submit_epoch_telemetry(const MatchState *ms,
                                   const int alive[TOTAL_SCRIPTS],
                                   int generation, float epoch_time,
                                   const char *pending_error)
{
    MatchStats mstats;
    memset(&mstats, 0, sizeof(mstats));
    mstats.match_number  = generation;
    mstats.total_matches = 0;            /* 0 == continuous (no fixed count) */
    mstats.duration      = epoch_time;
    mstats.llm_start     = ms->spawn_count[LLM_SCRIPT_IDX];
    mstats.llm_survivors = alive[LLM_SCRIPT_IDX];

    float hp_frac_sum = 0.0f;
    int   hp_count    = 0;
    for (int i = 0; i < g_bot_count; i++) {
        Bot *b = &g_bots[i];
        if (!b->active) continue;
        if (b->config.script_idx == LLM_SCRIPT_IDX) {
            hp_frac_sum += b->hp / b->config.max_hp;
            hp_count++;
        }
    }
    mstats.llm_avg_hp_frac = (hp_count > 0)
                             ? hp_frac_sum / (float)hp_count : 0.0f;

    update_get_llm_stats(&mstats.damage_dealt, &mstats.kills);
    update_get_runtime_error(mstats.runtime_error, sizeof(mstats.runtime_error));

    {
        LlmTelemetry tel;
        update_telemetry_get(&tel);
        mstats.think_frames         = tel.think_frames;
        mstats.enemy_visible_frames = tel.enemy_visible_frames;
        mstats.fire_frames          = tel.fire_frames;
        mstats.shots_fired          = tel.shots_fired;
        mstats.shots_hit            = tel.shots_hit;
        mstats.arena_bumps          = tel.arena_bumps;
        mstats.wall_bumps           = tel.wall_bumps;
        mstats.avg_nearest_dist     = (tel.nearest_dist_samples > 0)
            ? tel.nearest_dist_sum / (float)tel.nearest_dist_samples : 0.0f;
        mstats.visibility_frac      = (tel.think_frames > 0)
            ? (float)tel.enemy_visible_frames / (float)tel.think_frames : 0.0f;
        mstats.hit_rate             = (tel.shots_fired > 0)
            ? (float)tel.shots_hit / (float)tel.shots_fired : 0.0f;
    }

    if (ms->llm_load_error[0] != '\0')
        strncpy(mstats.script_error, ms->llm_load_error,
                sizeof(mstats.script_error) - 1);
    else if (pending_error && pending_error[0] != '\0')
        strncpy(mstats.script_error, pending_error,
                sizeof(mstats.script_error) - 1);

    int end_llm_alive = alive[LLM_SCRIPT_IDX];
    int end_non_llm   = 0;
    for (int s = 0; s < TOTAL_SCRIPTS; s++)
        if (s != LLM_SCRIPT_IDX && alive[s] > 0) end_non_llm++;

    const char *winner_name;
    if (end_non_llm == 0 && end_llm_alive > 0)
        winner_name = script_labels[LLM_SCRIPT_IDX];
    else if (end_llm_alive == 0 && end_non_llm > 0)
        winner_name = "non_llm";
    else
        winner_name = "draw";
    strncpy(mstats.winner_name, winner_name, sizeof(mstats.winner_name) - 1);

    llm_bot_submit_match(&mstats);
}

/* ----------------------------------------------------------------------- */
int main(void) {
    srand((unsigned)time(NULL));

    InitWindow(1920, 1080, "LlamaWars");
    SetTargetFPS(60);
    fx_init();
    lighting_init();

    colors_set_defaults(&g_colors);
    colors_load(&g_colors, COLORS_PATH);

    GameConfig gcfg;
    config_set_defaults(&gcfg);
    config_load(&gcfg, CFG_PATH);

    bool show_scan_lines = false;

    /* ================================================================== */
    /* Session loop — config screen → game → results → back to config     */
    /* ================================================================== */
    while (!WindowShouldClose()) {

    bool start_with_custom_prompt = show_config_screen(&gcfg);
    if (WindowShouldClose()) break;
    config_save(&gcfg, CFG_PATH);

    if (gcfg.use_llm && gcfg.reset_llm_bot)
        reset_llm_bot_script();

    float arena_half_x = gcfg.map_width  * 0.5f;
    float arena_half_z = gcfg.map_height * 0.5f;

    Camera3D cam_ortho = {
        .position   = {40.0f, 40.0f, 40.0f},
        .target     = { 0.0f,  0.0f,  0.0f},
        .up         = { 0.0f,  1.0f,  0.0f},
        .fovy       = 50.0f,
        .projection = CAMERA_PERSPECTIVE
    };
    Camera3D *camera = &cam_ortho;
    bool prompt_focused = false;
    char llm_prompt_buffer[sizeof(gcfg.llm_user_prompt)];
    strncpy(llm_prompt_buffer, gcfg.llm_user_prompt, sizeof(llm_prompt_buffer) - 1);
    llm_prompt_buffer[sizeof(llm_prompt_buffer) - 1] = '\0';

    if (gcfg.use_llm) {
        llm_bot_init(gcfg.llm_host, gcfg.llm_port,
                     script_paths[LLM_SCRIPT_IDX],
                     gcfg.llm_user_prompt);
        llm_bot_request_initial();
    }

    bool outer_done = false;
    char llm_pending_error[512] = {0};
    int  generation = 0;

        /* Continuous arena: build the world once, then run a single per-frame
         * loop. The LLM team is redeployed (reloading the newest validated
         * script from disk) only when the whole current generation is wiped or
         * the player presses R. Non-LLM teams respawn whenever wiped. */
        MatchState ms;
        unsigned wall_seed = (unsigned)time(NULL);
        match_setup(&ms, &gcfg, arena_half_x, arena_half_z, wall_seed, 0);
        update_reset_llm_stats();
        update_telemetry_reset();
        update_clear_runtime_error();

        int   alive[TOTAL_SCRIPTS] = {0};
        float epoch_time = 0.0f;

        /* -------------------------------------------------------------- */
        while (!WindowShouldClose() && !outer_done) {
            float dt = GetFrameTime();
            bool  force_deploy = false;
            epoch_time += dt;

            /* Prompt bar: click inside to focus, Enter to send, Esc to unfocus */
            if (gcfg.use_llm) {
                int sw = GetRenderWidth();
                int sh = GetRenderHeight();
                const int PBAR_H = 60;
                Rectangle bar_rect = {0, (float)(sh - PBAR_H), (float)sw, (float)PBAR_H};
                if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                    Vector2 mp = GetMousePosition();
                    prompt_focused = CheckCollisionPointRec(mp, bar_rect);
                }
                if (prompt_focused) {
                    bool shift_down = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
                    if (IsKeyPressed(KEY_ENTER) && !shift_down) {
                        strncpy(gcfg.llm_user_prompt, llm_prompt_buffer,
                                sizeof(gcfg.llm_user_prompt) - 1);
                        gcfg.llm_user_prompt[sizeof(gcfg.llm_user_prompt) - 1] = '\0';
                        config_save(&gcfg, CFG_PATH);
                        llm_bot_set_user_prompt(gcfg.llm_user_prompt);
                        llm_bot_request_prompt_refresh();
                        prompt_focused = false;
                    }
                    if (IsKeyPressed(KEY_ESCAPE)) {
                        strncpy(llm_prompt_buffer, gcfg.llm_user_prompt,
                                sizeof(llm_prompt_buffer) - 1);
                        llm_prompt_buffer[sizeof(llm_prompt_buffer) - 1] = '\0';
                        prompt_focused = false;
                    }
                }
            }

            if (!prompt_focused) {
                /* Camera controls */
                Vector3 pan_forward, pan_right;
                camera_ground_basis(camera, &pan_forward, &pan_right);
                Vector3 delta = {0};
                if (IsKeyDown(KEY_D)) delta = Vector3Add(delta, Vector3Scale(pan_right,    CAM_SPEED * dt));
                if (IsKeyDown(KEY_A)) delta = Vector3Add(delta, Vector3Scale(pan_right,   -CAM_SPEED * dt));
                if (IsKeyDown(KEY_W)) delta = Vector3Add(delta, Vector3Scale(pan_forward,  CAM_SPEED * dt));
                if (IsKeyDown(KEY_S)) delta = Vector3Add(delta, Vector3Scale(pan_forward, -CAM_SPEED * dt));
                if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
                    Vector2 md = GetMouseDelta();
                    float drag_scale = Vector3Distance(camera->position, camera->target) * 0.0025f;
                    delta = Vector3Add(delta, Vector3Scale(pan_right,   -md.x * drag_scale));
                    delta = Vector3Add(delta, Vector3Scale(pan_forward, -md.y * drag_scale));
                }
                camera->position = Vector3Add(camera->position, delta);
                camera->target   = Vector3Add(camera->target,   delta);

                if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT)) {
                    Vector2 md = GetMouseDelta();
                    orbit_camera_around_target(camera, -md.x * ORBIT_SPEED, -md.y * ORBIT_SPEED);
                }

                float wheel = GetMouseWheelMove();
                camera->fovy -= wheel * 3.0f;
                if (IsKeyDown(KEY_Q)) camera->fovy -= ZOOM_SPEED * dt;
                if (IsKeyDown(KEY_E)) camera->fovy += ZOOM_SPEED * dt;
                if (camera->fovy < ZOOM_MIN) camera->fovy = ZOOM_MIN;
                if (camera->fovy > ZOOM_MAX) camera->fovy = ZOOM_MAX;

                if (IsKeyDown(KEY_Z)) { camera->position.y += CAM_SPEED * dt; camera->target.y += CAM_SPEED * dt; }
                if (IsKeyDown(KEY_X)) { camera->position.y -= CAM_SPEED * dt; camera->target.y -= CAM_SPEED * dt; }
                clamp_camera_min_y(camera, wall_top_y());

                if (IsKeyPressed(KEY_F))      ToggleFullscreen();
                if (IsKeyPressed(KEY_T))      show_scan_lines = !show_scan_lines;
                if (IsKeyPressed(KEY_R))      force_deploy = true;
                if (IsKeyPressed(KEY_C)) {
                    camera->position   = (Vector3){40.0f, 40.0f, 40.0f};
                    camera->target     = (Vector3){ 0.0f,  0.0f,  0.0f};
                    camera->up         = (Vector3){ 0.0f,  1.0f,  0.0f};
                    camera->fovy       = 50.0f;
                    camera->projection = CAMERA_PERSPECTIVE;
                }
                if (IsKeyPressed(KEY_ESCAPE)) outer_done = true;
            }

            /* Simulation tick */
            update_scripts(g_bots, g_bot_count, dt);
            update_inertia(g_bots, g_bot_count, dt);
            update_movement(g_bots, g_bot_count, dt);
            update_projectiles(g_projs, &g_proj_count, g_bots, g_bot_count, dt);
            fx_update(dt);
            lighting_update(dt);

            /* Continuous self-improvement: keep the generator running
             * back-to-back in the background whenever it is idle. */
            if (gcfg.use_llm && !llm_bot_is_busy())
                llm_bot_request_continue();

            /* ---- Render ------------------------------------------------- */
            BeginDrawing();
                ClearBackground(g_colors.bg_clear);
                BeginMode3D(*camera);

                    /* Lit geometry: terrain + walls */
                    lighting_begin(*camera);

                    DrawPlane((Vector3){0,0,0},
                             (Vector2){gcfg.map_width, gcfg.map_height},
                             g_colors.terrain);

                    {
                        int        wn = walls_count();
                        const Wall *wv = walls_get();
                        int border_start = wn - 4;
                        for (int wi = 0; wi < wn; wi++) {
                            float wcx = wv[wi].x;
                            float wcz = wv[wi].z;
                            float ww  = wv[wi].hw * 2.0f;
                            float wd  = wv[wi].hd * 2.0f;
                            float wh  = wv[wi].height;
                            float wcy = wh * 0.5f;
                            Color fill = (wi >= border_start)
                                         ? g_colors.border_fill
                                         : g_colors.wall_fill;
                            draw_wall_tapered(wcx, wcy, wcz, ww, wh, wd, 0.2f, fill);
                        }
                    }

                    lighting_end();

                    /* Grid (unlit — lines have no normals) */
                    {
                        float gy = 0.005f;
                        float step = 2.0f;
                        for (float gx = -arena_half_x; gx <= arena_half_x; gx += step)
                            DrawLine3D((Vector3){gx, gy, -arena_half_z},
                                       (Vector3){gx, gy,  arena_half_z}, g_colors.grid);
                        for (float gz = -arena_half_z; gz <= arena_half_z; gz += step)
                            DrawLine3D((Vector3){-arena_half_x, gy, gz},
                                       (Vector3){ arena_half_x, gy, gz}, g_colors.grid);
                    }

                    /* Wall wireframe (unlit) */
                    {
                        int        wn = walls_count();
                        const Wall *wv = walls_get();
                        int border_start = wn - 4;
                        for (int wi = 0; wi < wn; wi++) {
                            float wcx = wv[wi].x;
                            float wcz = wv[wi].z;
                            float ww  = wv[wi].hw * 2.0f;
                            float wd  = wv[wi].hd * 2.0f;
                            float wh  = wv[wi].height;
                            float wcy = wh * 0.5f;
                            Color wire = (wi >= border_start)
                                         ? g_colors.border_wire
                                         : g_colors.wall_wire;
                            draw_wall_tapered_wires(wcx, wcy, wcz, ww, wh, wd, 0.2f, wire);
                        }
                    }

                    /* Bots */
                    for (int s = 0; s < TOTAL_SCRIPTS; s++) alive[s] = 0;

                    for (int i = 0; i < g_bot_count; i++) {
                        Bot *b = &g_bots[i];
                        if (!b->active) continue;
                        alive[b->config.script_idx]++;

                        Color col = {b->r, b->g, b->b, b->a};
                        draw_bot(b->x, b->z, col, &b->config, &b->inertia);

                        /* Scan lines (toggle with T) */
                        if (show_scan_lines) {
                            float sy = 0.08f;
                            for (int h = 0; h < b->inertia.scan_hit_count; h++) {
                                Color sc = b->inertia.scan_hit_type[h] == 0
                                           ? g_colors.scan_enemy
                                           : g_colors.scan_wall;
                                DrawLine3D(
                                    (Vector3){b->x, sy, b->z},
                                    (Vector3){b->inertia.scan_hit_x[h], sy,
                                              b->inertia.scan_hit_z[h]},
                                    sc);
                            }
                        }

                        /* Energy bar — perched just above the body roof. */
                        float bar_base_y = bot_body_top_y(&b->config) + CUBE_SIZE * 0.4f;
                        float bar_yaw    = camera_ground_yaw_deg(camera);
                        float base_hp    = 100.0f;
                        float frac       = (b->hp < base_hp)
                                           ? (b->hp / base_hp) : 1.0f;
                        float fill_w  = BAR_W * frac;
                        float fill_cx = -BAR_W * 0.5f + fill_w * 0.5f;
                        Color bar_col = frac > 0.6f ? g_colors.hp_full
                                      : frac > 0.3f ? g_colors.hp_mid
                                                     : g_colors.hp_low;
                        rlPushMatrix();
                            rlTranslatef(b->x, bar_base_y, b->z);
                            rlRotatef(bar_yaw, 0, 1, 0);
                            DrawCube((Vector3){0,0,0}, BAR_W, BAR_H, BAR_D,
                                     g_colors.hp_bg);
                            if (fill_w > 0.0f)
                                DrawCube((Vector3){fill_cx, 0, 0},
                                         fill_w, BAR_H, BAR_D, bar_col);
                        rlPopMatrix();

                        /* Armour bar — drawn whenever the body provides extra HP above the
                         * 100-point baseline (was: only when the dropped armour field was set). */
                        if (b->config.max_hp > base_hp) {
                            float armour_pool = b->config.max_hp - base_hp;
                            float armour_rem  = b->hp - base_hp;
                            if (armour_rem < 0.0f) armour_rem = 0.0f;
                            float afrac  = armour_rem / armour_pool;
                            float afillw = BAR_W * afrac;
                            float afillx = -BAR_W * 0.5f + afillw * 0.5f;
                            float ah     = BAR_H * 0.7f;
                            float ay     = bar_base_y + BAR_H + ah * 0.5f + 0.01f;
                            rlPushMatrix();
                                rlTranslatef(b->x, ay, b->z);
                                rlRotatef(bar_yaw, 0, 1, 0);
                                DrawCube((Vector3){0,0,0}, BAR_W, ah, BAR_D,
                                         g_colors.armour_bg);
                                if (afillw > 0.0f)
                                    DrawCube((Vector3){afillx,0,0},
                                             afillw, ah, BAR_D,
                                             g_colors.armour_fill);
                            rlPopMatrix();
                        }
                    }

                    /* Projectiles.
                     * NOTE: rlSetLineWidth() only calls glLineWidth(); it does
                     * NOT flush the deferred line batch. DrawLine3D vertices are
                     * batched and the width in effect at FLUSH time applies to
                     * every line in the batch. So we must flush the pending
                     * lines (grid/walls) BEFORE widening, draw all laser lines
                     * in one widened pass, then flush again and restore width —
                     * otherwise the thick laser width leaks onto the grid and
                     * the whole wireframe renders bold whenever lasers fire. */
                    float py = CUBE_SIZE * 0.3f;

                    /* Pass 1: thick laser lines, width-scoped via explicit flushes */
                    rlDrawRenderBatchActive();   /* flush grid/walls at width 1.0 */
                    rlSetLineWidth(2.0f);
                    for (int i = 0; i < g_proj_count; i++) {
                        Proj *p = &g_projs[i];
                        if (!p->active || p->weapon_type != WEAPON_LASER) continue;
                        float half = 0.80f;
                        DrawLine3D(
                            (Vector3){p->x - p->dir_x * half, py,
                                      p->z - p->dir_z * half},
                            (Vector3){p->x + p->dir_x * half, py,
                                      p->z + p->dir_z * half},
                            g_colors.laser);
                    }
                    rlDrawRenderBatchActive();   /* flush lasers while width is 2.0 */
                    rlSetLineWidth(1.0f);

                    /* Pass 2: non-laser projectiles as billboards */
                    for (int i = 0; i < g_proj_count; i++) {
                        Proj *p = &g_projs[i];
                        if (!p->active || p->weapon_type == WEAPON_LASER) continue;
                        Color col = {p->r, p->g, p->b, p->a};
                        DrawBillboard(*camera, default_billboard_texture(),
                                      (Vector3){p->x, py, p->z},
                                      CUBE_SIZE * 0.36f, col);
                    }

                    fx_draw();

                EndMode3D();

                /* HUD */
                const char *ctrl_hint =
                    "WASD/LMB-drag pan  RMB orbit  wheel/Q/E zoom  Z/X height  C reset cam  T scan  F full  R deploy  ESC quit";
                DrawText(ctrl_hint, 10, 10, 20, RAYWHITE);

                int hud_top = 34;
                if (gcfg.use_llm) {
                    LlmVisState hud_vis;
                    llm_bot_get_vis_state(&hud_vis);

                    char model_line[192];
                    snprintf(model_line, sizeof(model_line),
                             "Model: %s   Tok P/C/T: %d/%d/%d",
                             hud_vis.model[0] ? hud_vis.model : "?",
                             hud_vis.prompt_tokens,
                             hud_vis.completion_tokens,
                             hud_vis.total_tokens);
                    DrawText(model_line, 10, 34, 20, GRAY);
                    hud_top = 56;
                }

                for (int s = 0; s < TOTAL_SCRIPTS; s++) {
                    if (ms.spawn_count[s] == 0) continue;
                    DrawText(TextFormat("%-16s %d / %d",
                                        script_labels[s],
                                        alive[s], ms.spawn_count[s]),
                             10, hud_top + s * 22, 20, g_colors.team[s]);
                }
                if (gcfg.use_llm) {
                    DrawText(TextFormat("Generation: %d", generation),
                             10, hud_top + TOTAL_SCRIPTS * 22 + 4, 20, RAYWHITE);
                    DrawFPS(10, hud_top + TOTAL_SCRIPTS * 22 + 30);
                } else {
                    DrawFPS(10, hud_top + TOTAL_SCRIPTS * 22 + 4);
                }

                if (gcfg.use_llm) {
                    LlmVisState vis;
                    llm_bot_get_vis_state(&vis);
                    draw_llm_panel(&vis, generation);

                    /* Persistent prompt bar at the bottom */
                    int sw = GetRenderWidth();
                    int sh = GetRenderHeight();
                    const int PBAR_H   = 60;
                    const int LABEL_W  = 72;
                    const int HINT_W   = 360;

                    Color bar_bg     = prompt_focused
                                       ? (Color){28, 34, 46, 235}
                                       : (Color){16, 18, 26, 210};
                    Color bar_border = prompt_focused ? SKYBLUE : (Color){70, 75, 95, 255};

                    DrawRectangle(0, sh - PBAR_H, sw, PBAR_H, bar_bg);
                    DrawLine(0, sh - PBAR_H, sw, sh - PBAR_H, bar_border);

                    DrawText("Prompt:", 10, sh - PBAR_H + (PBAR_H - 20) / 2, 20,
                             prompt_focused ? SKYBLUE : LIGHTGRAY);

                    const char *hint = prompt_focused
                        ? "Enter=send  Shift+Enter=newline  Esc=cancel"
                        : "Click to edit  Enter=send";
                    DrawText(hint, sw - HINT_W, sh - PBAR_H + (PBAR_H - 18) / 2, 18,
                             prompt_focused ? (Color){120, 160, 200, 255} : DARKGRAY);

                    Rectangle text_rect = {(float)(LABEL_W), (float)(sh - PBAR_H + 4),
                                           (float)(sw - LABEL_W - HINT_W - 8),
                                           (float)(PBAR_H - 8)};
                    draw_multiline_prompt_box(text_rect, llm_prompt_buffer,
                                             (int)sizeof(llm_prompt_buffer),
                                             prompt_focused, 2, true);
                }

            EndDrawing();

            /* Continuous arena: every team keeps spawning. Each non-LLM team
             * respawns the instant it is individually wiped, so no side is ever
             * left lying dead waiting for the others. The LLM team respawns on
             * its own wipe (or on R), which is when the newest validated script
             * from disk is deployed. */
            {
                /* Keep each opponent team populated independently. */
                for (int s = 0; s < TOTAL_SCRIPTS; s++) {
                    if (s == LLM_SCRIPT_IDX) continue;
                    if (ms.spawn_count[s] > 0 && alive[s] == 0)
                        respawn_team(s, &gcfg, arena_half_x, arena_half_z);
                }

                bool llm_dead = (ms.spawn_count[LLM_SCRIPT_IDX] > 0) &&
                                (alive[LLM_SCRIPT_IDX] == 0);
                if (llm_dead || force_deploy) {
                    if (force_deploy) kill_team(LLM_SCRIPT_IDX);
                    if (gcfg.use_llm)
                        submit_epoch_telemetry(&ms, alive, generation, epoch_time,
                                               llm_pending_error);
                    /* respawn_team reloads bot_llm.py from disk, deploying the
                     * newest version generated in the background. */
                    respawn_team(LLM_SCRIPT_IDX, &gcfg,
                                 arena_half_x, arena_half_z);
                    if (gcfg.use_llm) {
                        generation++;
                        epoch_time = 0.0f;
                        update_reset_llm_stats();
                        update_telemetry_reset();
                        update_clear_runtime_error();

                        char gen_err[512] = {0};
                        if (llm_bot_poll_gen_error(gen_err, sizeof(gen_err)))
                            strncpy(llm_pending_error, gen_err,
                                    sizeof(llm_pending_error) - 1);
                        else if (llm_bot_poll_ready())
                            llm_pending_error[0] = '\0';
                    }
                }
            }
        } /* end main loop */

    match_teardown();

    if (gcfg.use_llm)
        llm_bot_shutdown();

    } /* end session loop */

    lighting_shutdown();
    fx_shutdown();
    scripting_finalize();
    CloseWindow();
    return 0;
}
