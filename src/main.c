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
    "scripts/bot_light.lua",
    "scripts/bot_skirmisher.lua",
    "scripts/bot_chaser.lua",
    "scripts/bot_duelist.lua",
    "scripts/bot_lancer.lua",
    "scripts/bot_fortress.lua",
    "scripts/bot_llm.lua",
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
    bool  use_llm;
    bool  opposite_corners;
    bool  auto_respawn;
    int   num_matches;
    int   match_duration;
    char  llm_host[80];
    int   llm_port;
} GameConfig;

static const int   DEFAULT_BOTS[TOTAL_SCRIPTS] = { 5, 2, 2, 2, 2, 2, 10 };
static const float DEFAULT_MAP_WIDTH            = 50.0f;
static const float DEFAULT_MAP_HEIGHT           = 20.0f;
static const int   DEFAULT_NUM_WALLS            = 2;
static const int   DEFAULT_MATCH_DURATION       = 50;
static const int   DEFAULT_NUM_MATCHES          = 10;

static float randf(float lo, float hi) {
    return lo + (float)rand() / (float)RAND_MAX * (hi - lo);
}

#define CFG_PATH "llama-wars.cfg"

static void config_set_defaults(GameConfig *cfg) {
    cfg->map_width        = DEFAULT_MAP_WIDTH;
    cfg->map_height       = DEFAULT_MAP_HEIGHT;
    cfg->num_walls        = DEFAULT_NUM_WALLS;
    cfg->use_llm          = false;
    cfg->opposite_corners = true;
    cfg->auto_respawn     = false;
    cfg->num_matches      = DEFAULT_NUM_MATCHES;
    cfg->match_duration   = DEFAULT_MATCH_DURATION;
    strncpy(cfg->llm_host, LLAMA_DEFAULT_HOST, sizeof(cfg->llm_host) - 1);
    cfg->llm_port = LLAMA_DEFAULT_PORT;
    for (int s = 0; s < TOTAL_SCRIPTS; s++)
        cfg->bots_per_type[s] = DEFAULT_BOTS[s];
}

static bool config_load(GameConfig *cfg, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return false;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char key[64] = {0};
        char val[128] = {0};
        if (sscanf(line, " %63[a-z_] = %127[^\n]", key, val) != 2) continue;
        char *comment = strchr(val, '#');
        if (comment) *comment = '\0';
        while (strlen(val) > 0 && val[strlen(val)-1] == ' ') val[strlen(val)-1] = '\0';

        if      (strcmp(key, "map_width")       == 0) cfg->map_width  = (float)atoi(val);
        else if (strcmp(key, "map_height")      == 0) cfg->map_height = (float)atoi(val);
        else if (strcmp(key, "walls")           == 0) cfg->num_walls  = atoi(val);
        else if (strcmp(key, "spawn_mode")      == 0) cfg->opposite_corners = (strstr(val, "corner") != NULL);
        else if (strcmp(key, "game_mode")       == 0) cfg->auto_respawn = (strstr(val, "respawn") != NULL);
        else if (strcmp(key, "num_matches")     == 0) cfg->num_matches = atoi(val);
        else if (strcmp(key, "match_duration")  == 0) cfg->match_duration = atoi(val);
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
    }
    fclose(f);
    return true;
}

static void config_save(const GameConfig *cfg, const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return;

    fprintf(f, "map_width        = %-4d # 10-200\n",  (int)cfg->map_width);
    fprintf(f, "map_height       = %-4d # 10-200\n",  (int)cfg->map_height);
    fprintf(f, "walls            = %-4d # 0-40\n",    cfg->num_walls);
    fprintf(f, "spawn_mode       = %-8s # corners | random\n",
            cfg->opposite_corners ? "corners" : "random");
    fprintf(f, "game_mode        = %-8s # match | respawn\n",
            cfg->auto_respawn ? "respawn" : "match");
    fprintf(f, "num_matches      = %-4d # 1-100\n",   cfg->num_matches);
    fprintf(f, "match_duration   = %-4d # 30-600\n",  cfg->match_duration);
    fprintf(f, "\n");
    fprintf(f, "bot_light        = %-4d # 0-30\n", cfg->bots_per_type[0]);
    fprintf(f, "bot_skirmisher   = %-4d # 0-30\n", cfg->bots_per_type[1]);
    fprintf(f, "bot_chaser       = %-4d # 0-30\n", cfg->bots_per_type[2]);
    fprintf(f, "bot_duelist      = %-4d # 0-30\n", cfg->bots_per_type[3]);
    fprintf(f, "bot_lancer       = %-4d # 0-30\n", cfg->bots_per_type[4]);
    fprintf(f, "bot_fortress     = %-4d # 0-30\n", cfg->bots_per_type[5]);
    fprintf(f, "bot_llm          = %-4d # 0-30\n", cfg->bots_per_type[6]);
    fprintf(f, "\n");
    fprintf(f, "llm_host         = %s\n", cfg->llm_host);
    fprintf(f, "llm_port         = %-4d # 1-65535\n", cfg->llm_port);

    fclose(f);
}

/* ----------------------------------------------------------------------- */
static void show_config_screen(GameConfig *cfg) {
    /* Probe llama-server health each time the config screen is shown */
    bool server_available = llama_server_healthy(cfg->llm_host, cfg->llm_port);
    if (server_available && !cfg->use_llm)
        cfg->use_llm = true;

    const int SW      = GetRenderWidth();
    const int SH      = GetRenderHeight();
    const int PW      = 520;
    const int ROW_H   = 34;
    const int ROWS    = TOTAL_SCRIPTS + 12;
    const int PH      = 60 + ROWS * ROW_H + 16;
    const int PX      = (SW - PW) / 2;
    const int PY      = (SH - PH) / 2 > 10 ? (SH - PH) / 2 : 10;
    const int LBL_W   = 190;
    const int CTL_X   = PX + LBL_W + 10;
    const int CTL_W   = PW - LBL_W - 30;
    const float FONT_SZ = 20.0f;

    bool edit[5 + TOTAL_SCRIPTS];
    for (int i = 0; i < 5 + TOTAL_SCRIPTS; i++) edit[i] = false;

    int map_width_int  = (int)cfg->map_width;
    int map_height_int = (int)cfg->map_height;

    GuiSetStyle(DEFAULT, TEXT_SIZE, (int)FONT_SZ);

    while (!WindowShouldClose()) {
        BeginDrawing();
        ClearBackground(g_colors.bg_config);

        GuiPanel((Rectangle){(float)PX, (float)PY, (float)PW, (float)PH},
                 "LlamaWars — Configuration");

        int row = 0;
#define ROW_Y  (PY + 48 + row * ROW_H)

        /* Map width (X axis — the long side for opposite-corners) */
        GuiLabel((Rectangle){(float)(PX + 10), (float)ROW_Y, (float)LBL_W, (float)ROW_H},
                 "Map width (10-200)");
        if (GuiSpinner((Rectangle){(float)CTL_X, (float)ROW_Y, (float)CTL_W, (float)(ROW_H - 4)},
                       NULL, &map_width_int, 10, 200, edit[0]))
            edit[0] = !edit[0];
        cfg->map_width = (float)map_width_int;
        row++;

        /* Map height (Z axis) */
        GuiLabel((Rectangle){(float)(PX + 10), (float)ROW_Y, (float)LBL_W, (float)ROW_H},
                 "Map height (10-200)");
        if (GuiSpinner((Rectangle){(float)CTL_X, (float)ROW_Y, (float)CTL_W, (float)(ROW_H - 4)},
                       NULL, &map_height_int, 10, 200, edit[1]))
            edit[1] = !edit[1];
        cfg->map_height = (float)map_height_int;
        row++;

        /* Wall count */
        GuiLabel((Rectangle){(float)(PX + 10), (float)ROW_Y, (float)LBL_W, (float)ROW_H},
                 "Walls (0-40)");
        if (GuiSpinner((Rectangle){(float)CTL_X, (float)ROW_Y, (float)CTL_W, (float)(ROW_H - 4)},
                       NULL, &cfg->num_walls, 0, 40, edit[2]))
            edit[2] = !edit[2];
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

        /* Game mode toggle */
        GuiLabel((Rectangle){(float)(PX + 10), (float)ROW_Y, (float)LBL_W, (float)ROW_H},
                 "Game mode");
        {
            const char *mode_text = cfg->auto_respawn
                                    ? "Auto Respawn" : "Match";
            bool ar = cfg->auto_respawn;
            GuiToggle((Rectangle){(float)CTL_X, (float)ROW_Y + 2,
                                  (float)CTL_W, (float)(ROW_H - 6)},
                      mode_text, &ar);
            cfg->auto_respawn = ar;
        }
        row++;

        /* LLM section */
        {
            bool chk = cfg->use_llm;
            if (!server_available) GuiDisable();
            if (GuiCheckBox((Rectangle){(float)CTL_X, (float)ROW_Y + 6,
                                        (float)(ROW_H - 12), (float)(ROW_H - 12)},
                            "Use LLM AI", &chk))
                cfg->use_llm = chk;
            if (!server_available) {
                GuiEnable();
                GuiLabel((Rectangle){(float)(PX + 10), (float)ROW_Y,
                                     (float)LBL_W, (float)ROW_H},
                         "LLM Server");
                GuiLabel((Rectangle){(float)(CTL_X + ROW_H), (float)ROW_Y,
                                     260.0f, (float)ROW_H},
                         "(start llama-server first)");
            } else {
                GuiLabel((Rectangle){(float)(PX + 10), (float)ROW_Y,
                                     (float)LBL_W, (float)ROW_H},
                         "LLM Server");
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

        /* Number of matches */
        if (!cfg->use_llm) GuiDisable();
        GuiLabel((Rectangle){(float)(PX + 10), (float)ROW_Y, (float)LBL_W, (float)ROW_H},
                 "Number of matches");
        if (GuiSpinner((Rectangle){(float)CTL_X, (float)ROW_Y, (float)CTL_W, (float)(ROW_H - 4)},
                       NULL, &cfg->num_matches, 1, 100, edit[3]))
            edit[3] = !edit[3];
        if (!cfg->use_llm) GuiEnable();
        row++;

        /* Match duration */
        if (!cfg->use_llm) GuiDisable();
        GuiLabel((Rectangle){(float)(PX + 10), (float)ROW_Y, (float)LBL_W, (float)ROW_H},
                 "Match duration (s)");
        if (GuiSpinner((Rectangle){(float)CTL_X, (float)ROW_Y, (float)CTL_W, (float)(ROW_H - 4)},
                       NULL, &cfg->match_duration, 30, 600, edit[4]))
            edit[4] = !edit[4];
        if (!cfg->use_llm) GuiEnable();
        row++;

        /* Divider */
        GuiLine((Rectangle){(float)(PX + 10), (float)(ROW_Y - 4),
                             (float)(PW - 20), 1}, "Bots per script");
        row++;

        /* Bot count spinners */
        for (int s = 0; s < TOTAL_SCRIPTS; s++) {
            GuiLabel((Rectangle){(float)(PX + 10), (float)ROW_Y,
                                 (float)LBL_W, (float)ROW_H},
                     script_labels[s]);
            if (GuiSpinner((Rectangle){(float)CTL_X, (float)ROW_Y,
                                       (float)CTL_W, (float)(ROW_H - 4)},
                           NULL, &cfg->bots_per_type[s], 0, 30, edit[s + 5]))
                edit[s + 5] = !edit[s + 5];

            if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
                Rectangle r = {(float)CTL_X, (float)ROW_Y,
                               (float)CTL_W, (float)(ROW_H - 4)};
                if (!CheckCollisionPointRec(GetMousePosition(), r))
                    edit[s + 5] = false;
            }
            row++;
        }

        /* Dismiss spinners on outside click */
        if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
            for (int i = 0; i < 5; i++) {
                int ry = PY + 48 + i * ROW_H;
                Rectangle r = {(float)CTL_X, (float)ry, (float)CTL_W, (float)(ROW_H-4)};
                if (!CheckCollisionPointRec(GetMousePosition(), r))
                    edit[i] = false;
            }
        }

        /* Summary */
        int total = 0;
        for (int s = 0; s < TOTAL_SCRIPTS; s++) total += cfg->bots_per_type[s];
        GuiLabel((Rectangle){(float)(PX + 10), (float)ROW_Y,
                              (float)(PW - 20), (float)ROW_H},
                 TextFormat("Total: %d bots   Map: %d x %d   Walls: %d",
                            total, map_width_int, map_height_int, cfg->num_walls));
        row++;

        /* Start button */
        if (GuiButton((Rectangle){(float)(PX + PW / 2 - 80), (float)(ROW_Y + 4),
                                  160.0f, (float)(ROW_H - 4)},
                      "Start Game")) {
            EndDrawing();
            break;
        }

#undef ROW_Y

        EndDrawing();
    }
}

/* ----------------------------------------------------------------------- */
static void draw_weapon_local(WeaponType wt, float side_x, Color col) {
    float ww, wh, wd;
    if (wt == WEAPON_MACHINE_GUN) {
        ww = CUBE_SIZE * 0.20f; wh = CUBE_SIZE * 0.15f; wd = CUBE_SIZE * 0.60f;
    } else if (wt == WEAPON_AUTO_CANNON) {
        ww = CUBE_SIZE * 0.25f; wh = CUBE_SIZE * 0.25f; wd = CUBE_SIZE * 0.45f;
    } else {
        ww = CUBE_SIZE * 0.12f; wh = CUBE_SIZE * 0.12f; wd = CUBE_SIZE * 0.80f;
    }
    DrawCube((Vector3){side_x, 0, wd * 0.1f}, ww, wh, wd, col);
    DrawCubeWires((Vector3){side_x, 0, wd * 0.1f}, ww, wh, wd, BLACK);
}

static void draw_bot(float cx, float cz, Color color,
                     const BotConfig *cfg,
                     float body_angle, float turret_angle)
{
    float bs    = CUBE_SIZE * cfg->body_scale;
    float trk_h = bs * 0.5f;
    float trk_y = trk_h * 0.5f;
    float bod_y = trk_h + bs * 0.5f;
    float trk_sz = bs * 2.0f;

    float body_deg = 90.0f - body_angle * RAD2DEG_F;
    rlPushMatrix();
        rlTranslatef(cx, 0.0f, cz);
        rlRotatef(body_deg, 0.0f, 1.0f, 0.0f);

        Color trk_col = g_colors.bot_tread;
        DrawCube((Vector3){0, trk_y, 0}, trk_sz, trk_h, trk_sz, trk_col);
        DrawCubeWires((Vector3){0, trk_y, 0}, trk_sz, trk_h, trk_sz, BLACK);

        DrawCube((Vector3){0, bod_y, 0}, bs, bs, bs, color);
        DrawCubeWires((Vector3){0, bod_y, 0}, bs, bs, bs, BLACK);
    rlPopMatrix();

    float turret_deg = 90.0f - turret_angle * RAD2DEG_F;
    float half = bs * 0.5f;

    float lw = (cfg->left_weapon  == WEAPON_MACHINE_GUN) ? CUBE_SIZE * 0.20f :
               (cfg->left_weapon  == WEAPON_AUTO_CANNON)  ? CUBE_SIZE * 0.25f :
                                                             CUBE_SIZE * 0.50f;
    float rw = (cfg->right_weapon == WEAPON_MACHINE_GUN) ? CUBE_SIZE * 0.20f :
               (cfg->right_weapon == WEAPON_AUTO_CANNON)  ? CUBE_SIZE * 0.25f :
                                                             CUBE_SIZE * 0.50f;

    Color wcolor = g_colors.bot_weapon;
    rlPushMatrix();
        rlTranslatef(cx, bod_y, cz);
        rlRotatef(turret_deg, 0.0f, 1.0f, 0.0f);
        draw_weapon_local(cfg->left_weapon,  -(half + lw * 0.5f + 0.02f), wcolor);
        draw_weapon_local(cfg->right_weapon, +(half + rw * 0.5f + 0.02f), wcolor);
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

        lua_State *L = scripting_load(script_paths[script_idx]);
        BotConfig cfg;
        if (L) {
            scripting_call_init(L, &cfg);
        } else {
            cfg.left_weapon  = WEAPON_AUTO_CANNON;
            cfg.right_weapon = WEAPON_AUTO_CANNON;
            cfg.armour       = 0;
            cfg.max_hp       = 100.0f;
            cfg.max_speed    = 5.0f;
            cfg.body_scale   = 1.0f;
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
        bot->L         = L;
    }
}

static void match_setup(MatchState *ms, const GameConfig *gcfg,
                        float arena_half_x, float arena_half_z,
                        unsigned wall_seed)
{
    g_bot_count  = 0;
    g_proj_count = 0;
    memset(g_bots,  0, sizeof(g_bots));
    memset(g_projs, 0, sizeof(g_projs));

    update_set_arena(arena_half_x, arena_half_z);
    walls_generate(arena_half_x, arena_half_z, gcfg->num_walls, wall_seed);
    walls_add_border(arena_half_x, arena_half_z);
    scripting_init();

    ms->llm_load_error[0] = '\0';
    for (int s = 0; s < TOTAL_SCRIPTS; s++) {
        int n = gcfg->bots_per_type[s];
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

            lua_State *L = scripting_load(script_paths[s]);
            if (!L && s == LLM_SCRIPT_IDX && ms->llm_load_error[0] == '\0') {
                const char *err = scripting_get_last_error();
                if (err)
                    snprintf(ms->llm_load_error, sizeof(ms->llm_load_error), "%s", err);
            }
            BotConfig cfg;
            if (L) {
                scripting_call_init(L, &cfg);
            } else {
                cfg.left_weapon  = WEAPON_AUTO_CANNON;
                cfg.right_weapon = WEAPON_AUTO_CANNON;
                cfg.armour       = 0;
                cfg.max_hp       = 100.0f;
                cfg.max_speed    = 5.0f;
                cfg.body_scale   = 1.0f;
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
            bot->L         = L;
        }
    }
}

static void match_teardown(void) {
    scripting_shutdown();
    g_bot_count  = 0;
    g_proj_count = 0;
}

/* ----------------------------------------------------------------------- */
static void draw_llm_panel(const LlmVisState *vis,
                            int match_idx, int total_matches,
                            float match_time, float match_duration)
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

    DrawText("MCH>", cx,      cy, FSZ, dim);
    DrawText(TextFormat("%d / %d   T %.0fs",
                        match_idx + 1, total_matches,
                        (double)(match_duration - match_time)),
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
static void show_match_result(const MatchStats *ms, bool llm_busy, bool is_last) {
    float show_for = is_last ? 1.0f : 3.0f;
    float elapsed  = 0.0f;
    while (elapsed < show_for && !WindowShouldClose()) {
        elapsed += GetFrameTime();

        BeginDrawing();
        ClearBackground(g_colors.bg_results);

        int cx = GetRenderWidth()  / 2;
        int cy = GetRenderHeight() / 2;

        DrawText(TextFormat("MATCH %d / %d COMPLETE",
                            ms->match_number, ms->total_matches),
                 cx - 220, cy - 120, 38, WHITE);
        DrawText(TextFormat("Winner: %s", ms->winner_name),
                 cx - 220, cy - 68,  30, YELLOW);
        DrawText(TextFormat("Duration: %.1f s   |   bot_llm: %d / %d survived",
                            (double)ms->duration,
                            ms->llm_survivors, ms->llm_start),
                 cx - 220, cy - 24, 22, LIGHTGRAY);
        DrawText(TextFormat("Damage dealt: %.0f   |   Kills: %d",
                            (double)ms->damage_dealt, ms->kills),
                 cx - 220, cy + 10, 22, LIGHTGRAY);

        if (llm_busy)
            DrawText("LLM optimising next script ...", cx - 220, cy + 55, 24, SKYBLUE);
        else if (ms->match_number < ms->total_matches)
            DrawText("Script updated — loading next match!", cx - 220, cy + 55, 24, GREEN);

        float frac     = 1.0f - (elapsed / show_for);
        int   bar_full = GetRenderWidth() - 100;
        DrawRectangle(50, cy + 105, (int)(bar_full * frac), 6, RAYWHITE);
        const char *next_lbl = is_last ? "Results screen in 1 s"
                                       : TextFormat("Next match in %d s",
                                                    (int)(show_for - elapsed) + 1);
        DrawText(next_lbl, cx - 70, cy + 118, 20, GRAY);

        EndDrawing();
    }
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

    Vector3 pan_right   = Vector3Normalize((Vector3){1.0f, 0.0f, -1.0f});
    Vector3 pan_forward = Vector3Normalize((Vector3){1.0f, 0.0f,  1.0f});
    bool show_scan_lines = false;

    /* ================================================================== */
    /* Session loop — config screen → game → results → back to config     */
    /* ================================================================== */
    while (!WindowShouldClose()) {

    show_config_screen(&gcfg);
    if (WindowShouldClose()) break;
    config_save(&gcfg, CFG_PATH);

    float arena_half_x = gcfg.map_width  * 0.5f;
    float arena_half_z = gcfg.map_height * 0.5f;

    Camera3D cam_ortho = {
        .position   = {40.0f, 40.0f, 40.0f},
        .target     = { 0.0f,  0.0f,  0.0f},
        .up         = { 0.0f,  1.0f,  0.0f},
        .fovy       = 45.0f,
        .projection = CAMERA_ORTHOGRAPHIC
    };
    Camera3D cam_third = {
        .position   = {arena_half_x * 1.2f, arena_half_z * 0.9f, 0.0f},
        .target     = {0.0f, 0.0f, 0.0f},
        .up         = {0.0f, 1.0f, 0.0f},
        .fovy       = 60.0f,
        .projection = CAMERA_PERSPECTIVE
    };
    int cam_mode = 0;
    Camera3D *camera = &cam_ortho;

    if (gcfg.use_llm) {
        llm_bot_init(gcfg.llm_host, gcfg.llm_port,
                     script_paths[LLM_SCRIPT_IDX]);
    }

    int  match_idx  = 0;
    bool outer_done = false;
    char match_winners[64][32];
    memset(match_winners, 0, sizeof(match_winners));
    char llm_pending_error[512] = {0};

    while (!WindowShouldClose() && !outer_done) {

        MatchState ms;
        unsigned wall_seed = (unsigned)time(NULL) + (unsigned)(match_idx * 31337);
        match_setup(&ms, &gcfg, arena_half_x, arena_half_z, wall_seed);
        update_reset_llm_stats();
        update_clear_runtime_error();

        float match_time    = 0.0f;
        bool  match_over    = false;
        bool  restart_match = false;
        int   alive[TOTAL_SCRIPTS] = {0};
        int   rounds_llm    = 0;
        int   rounds_nonllm = 0;

        int teams_with_bots = 0;
        for (int s = 0; s < TOTAL_SCRIPTS; s++)
            if (ms.spawn_count[s] > 0) teams_with_bots++;

        /* -------------------------------------------------------------- */
        while (!WindowShouldClose() && !match_over) {
            float dt = GetFrameTime();
            match_time += dt;

            /* Camera mode toggle */
            if (IsKeyPressed(KEY_C)) {
                cam_mode = 1 - cam_mode;
                camera = cam_mode ? &cam_third : &cam_ortho;
            }

            /* Camera controls */
            Vector3 delta = {0};
            if (IsKeyDown(KEY_D)) delta = Vector3Add(delta, Vector3Scale(pan_right,    CAM_SPEED * dt));
            if (IsKeyDown(KEY_A)) delta = Vector3Add(delta, Vector3Scale(pan_right,   -CAM_SPEED * dt));
            if (IsKeyDown(KEY_W)) delta = Vector3Add(delta, Vector3Scale(pan_forward, -CAM_SPEED * dt));
            if (IsKeyDown(KEY_S)) delta = Vector3Add(delta, Vector3Scale(pan_forward,  CAM_SPEED * dt));
            camera->position = Vector3Add(camera->position, delta);
            camera->target   = Vector3Add(camera->target,   delta);

            float wheel = GetMouseWheelMove();
            camera->fovy -= wheel * 3.0f;
            if (IsKeyDown(KEY_Q)) camera->fovy -= ZOOM_SPEED * dt;
            if (IsKeyDown(KEY_E)) camera->fovy += ZOOM_SPEED * dt;
            if (camera->fovy < ZOOM_MIN) camera->fovy = ZOOM_MIN;
            if (camera->fovy > ZOOM_MAX) camera->fovy = ZOOM_MAX;

            if (IsKeyDown(KEY_Z)) { camera->position.y += CAM_SPEED * dt; camera->target.y += CAM_SPEED * dt; }
            if (IsKeyDown(KEY_X)) { camera->position.y -= CAM_SPEED * dt; camera->target.y -= CAM_SPEED * dt; }

            if (IsKeyPressed(KEY_F))      ToggleFullscreen();
            if (IsKeyPressed(KEY_T))      show_scan_lines = !show_scan_lines;
            if (IsKeyPressed(KEY_R))      { restart_match = true; match_over = true; }
            if (IsKeyPressed(KEY_ESCAPE)) { outer_done    = true; match_over = true; }

            /* Simulation tick */
            update_scripts(g_bots, g_bot_count, dt);
            update_inertia(g_bots, g_bot_count, dt);
            update_movement(g_bots, g_bot_count, dt);
            update_projectiles(g_projs, &g_proj_count, g_bots, g_bot_count, dt);
            fx_update(dt);
            lighting_update(dt);

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
                            DrawCube((Vector3){wcx, wcy, wcz}, ww, wh, wd, fill);
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
                            DrawCubeWires((Vector3){wcx, wcy, wcz}, ww, wh, wd, wire);
                        }
                    }

                    /* Bots */
                    for (int s = 0; s < TOTAL_SCRIPTS; s++) alive[s] = 0;

                    for (int i = 0; i < g_bot_count; i++) {
                        Bot *b = &g_bots[i];
                        if (!b->active) continue;
                        alive[b->config.script_idx]++;

                        Color col = {b->r, b->g, b->b, b->a};
                        draw_bot(b->x, b->z, col, &b->config,
                                 b->inertia.body_angle, b->inertia.turret_angle);

                        /* Scan lines (toggle with T) */
                        if (show_scan_lines) {
                            Color scan_col = {b->r / 6, b->g / 6, b->b / 6, 40};
                            float sy = 0.08f;
                            for (int h = 0; h < b->inertia.scan_hit_count; h++)
                                DrawLine3D(
                                    (Vector3){b->x, sy, b->z},
                                    (Vector3){b->inertia.scan_hit_x[h], sy,
                                              b->inertia.scan_hit_z[h]},
                                    scan_col);
                        }

                        /* Energy bar */
                        float bar_base_y = BAR_Y * b->config.body_scale;
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
                            rlRotatef(45.0f, 0, 1, 0);
                            DrawCube((Vector3){0,0,0}, BAR_W, BAR_H, BAR_D,
                                     g_colors.hp_bg);
                            if (fill_w > 0.0f)
                                DrawCube((Vector3){fill_cx, 0, 0},
                                         fill_w, BAR_H, BAR_D, bar_col);
                        rlPopMatrix();

                        /* Armour bar */
                        if (b->config.armour > 0) {
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
                                rlRotatef(45.0f, 0, 1, 0);
                                DrawCube((Vector3){0,0,0}, BAR_W, ah, BAR_D,
                                         g_colors.armour_bg);
                                if (afillw > 0.0f)
                                    DrawCube((Vector3){afillx,0,0},
                                             afillw, ah, BAR_D,
                                             g_colors.armour_fill);
                            rlPopMatrix();
                        }
                    }

                    /* Projectiles */
                    for (int i = 0; i < g_proj_count; i++) {
                        Proj *p = &g_projs[i];
                        if (!p->active) continue;
                        float py = CUBE_SIZE * 0.3f;
                        if (p->weapon_type == WEAPON_LASER) {
                            float half = 0.80f;
                            DrawLine3D(
                                (Vector3){p->x - p->dir_x * half, py,
                                          p->z - p->dir_z * half},
                                (Vector3){p->x + p->dir_x * half, py,
                                          p->z + p->dir_z * half},
                                g_colors.laser);
                        } else {
                            Color col = {p->r, p->g, p->b, p->a};
                            DrawSphere((Vector3){p->x, py, p->z},
                                       CUBE_SIZE * 0.18f, col);
                        }
                    }

                    fx_draw();

                EndMode3D();

                /* HUD */
                const char *ctrl_hint = gcfg.use_llm
                    ? TextFormat("WASD pan  Q/E zoom  Z/X height  C camera  T scan  F full  R restart  ESC quit"
                                 "   Match %d/%d  %.0fs left",
                                 match_idx + 1, gcfg.num_matches,
                                 (double)(gcfg.match_duration - match_time))
                    : "WASD pan  Q/E zoom  Z/X height  C camera  T scan  F full  R restart  ESC quit";
                DrawText(ctrl_hint, 10, 10, 20, RAYWHITE);

                for (int s = 0; s < TOTAL_SCRIPTS; s++) {
                    if (ms.spawn_count[s] == 0) continue;
                    DrawText(TextFormat("%-16s %d / %d",
                                        script_labels[s],
                                        alive[s], ms.spawn_count[s]),
                             10, 34 + s * 22, 20, g_colors.team[s]);
                }
                if (gcfg.auto_respawn) {
                    DrawText(TextFormat("Rounds  —  LLM: %d   Others: %d",
                                        rounds_llm, rounds_nonllm),
                             10, 34 + TOTAL_SCRIPTS * 22 + 4, 20, RAYWHITE);
                    DrawFPS(10, 34 + TOTAL_SCRIPTS * 22 + 30);
                } else {
                    DrawFPS(10, 34 + TOTAL_SCRIPTS * 22 + 4);
                }

                if (gcfg.use_llm) {
                    LlmVisState vis;
                    llm_bot_get_vis_state(&vis);
                    draw_llm_panel(&vis, match_idx, gcfg.num_matches,
                                    match_time, (float)gcfg.match_duration);
                }

            EndDrawing();

            /* Check match-end conditions */
            {
                int llm_alive   = alive[LLM_SCRIPT_IDX];
                int non_llm_alive = 0;
                for (int s = 0; s < TOTAL_SCRIPTS; s++)
                    if (s != LLM_SCRIPT_IDX && alive[s] > 0) non_llm_alive++;

                if (match_time > 3.0f && teams_with_bots > 1) {
                    if (llm_alive == 0 || non_llm_alive == 0) {
                        if (gcfg.auto_respawn) {
                            if (llm_alive == 0) {
                                rounds_nonllm++;
                                respawn_team(LLM_SCRIPT_IDX, &gcfg,
                                             arena_half_x, arena_half_z);
                            }
                            if (non_llm_alive == 0) {
                                rounds_llm++;
                                for (int s = 0; s < TOTAL_SCRIPTS; s++)
                                    if (s != LLM_SCRIPT_IDX && ms.spawn_count[s] > 0)
                                        respawn_team(s, &gcfg,
                                                     arena_half_x, arena_half_z);
                            }
                        } else {
                            match_over = true;
                        }
                    }
                }
                if (gcfg.use_llm &&
                    match_time >= (float)gcfg.match_duration)
                    match_over = true;
            }
        } /* end inner loop */

        /* -------------------------------------------------------------- */
        if (restart_match || outer_done) {
            match_teardown();
            continue;
        }

        if (gcfg.use_llm) {
            MatchStats mstats;
            memset(&mstats, 0, sizeof(mstats));
            mstats.match_number   = match_idx + 1;
            mstats.total_matches  = gcfg.num_matches;
            mstats.duration       = match_time;
            mstats.llm_start      = ms.spawn_count[LLM_SCRIPT_IDX];
            mstats.llm_survivors  = alive[LLM_SCRIPT_IDX];

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

            if (ms.llm_load_error[0] != '\0')
                strncpy(mstats.script_error, ms.llm_load_error,
                        sizeof(mstats.script_error) - 1);
            else
                strncpy(mstats.script_error, llm_pending_error,
                        sizeof(mstats.script_error) - 1);

            /* Two-sided match: LLM vs non-LLM coalition.
             * Winner is "bot_llm" if all non-LLM bots are gone, "non_llm" if
             * all LLM bots are gone, or "timeout" if neither side is wiped. */
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
                winner_name = "timeout";
            strncpy(mstats.winner_name, winner_name, sizeof(mstats.winner_name) - 1);

            if (match_idx < 64)
                strncpy(match_winners[match_idx], mstats.winner_name,
                        sizeof(match_winners[0]) - 1);

            llm_bot_submit_match(&mstats);

            bool is_last = (match_idx + 1 >= gcfg.num_matches);
            show_match_result(&mstats, llm_bot_is_busy(), is_last);

            {
                char gen_err[512] = {0};
                if (llm_bot_poll_gen_error(gen_err, sizeof(gen_err)))
                    strncpy(llm_pending_error, gen_err,
                            sizeof(llm_pending_error) - 1);
                else if (llm_bot_poll_ready())
                    llm_pending_error[0] = '\0';
            }

            match_idx++;
            if (match_idx >= gcfg.num_matches)
                outer_done = true;
        } else {
            match_idx++;
            outer_done = true;
        }

        match_teardown();
    } /* end outer loop */

    /* ================================================================== */
    int total_played = match_idx;
    if (total_played > 0 && !WindowShouldClose()) {

        int win_count[TOTAL_SCRIPTS] = {0};
        int timeouts = 0;
        for (int m = 0; m < total_played && m < 64; m++) {
            bool found = false;
            for (int s = 0; s < TOTAL_SCRIPTS; s++) {
                if (strcmp(match_winners[m], script_labels[s]) == 0) {
                    win_count[s]++;
                    found = true;
                    break;
                }
            }
            if (!found) timeouts++;
        }

        printf("\n=== SESSION RESULTS (%d matches) ===\n", total_played);
        for (int m = 0; m < total_played && m < 64; m++)
            printf("  Match %2d: %s\n", m + 1, match_winners[m]);
        printf("\n--- Wins ---\n");
        for (int s = 0; s < TOTAL_SCRIPTS; s++)
            if (win_count[s] > 0)
                printf("  %-18s %d\n", script_labels[s], win_count[s]);
        if (timeouts > 0)
            printf("  %-18s %d\n", "timeout", timeouts);
        printf("====================================\n\n");
        fflush(stdout);

        while (!WindowShouldClose() && !IsKeyPressed(KEY_ESCAPE) &&
               !IsKeyPressed(KEY_ENTER) && !IsKeyPressed(KEY_SPACE)) {

            BeginDrawing();
            ClearBackground(g_colors.bg_results);

            int sw = GetRenderWidth();
            int sh = GetRenderHeight();
            int cx = sw / 2;
            int cy = 60;

            DrawText(TextFormat("SESSION COMPLETE  —  %d matches", total_played),
                     cx - MeasureText(TextFormat("SESSION COMPLETE  —  %d matches",
                                                 total_played), 36) / 2,
                     cy, 36, WHITE);
            cy += 58;

            int cols   = 3;
            int col_w  = sw / cols;
            for (int m = 0; m < total_played && m < 64; m++) {
                int col = m % cols;
                int row = m / cols;
                int tx  = col * col_w + 40;
                int ty  = cy + row * 26;

                Color wc = LIGHTGRAY;
                for (int s = 0; s < TOTAL_SCRIPTS; s++) {
                    if (strcmp(match_winners[m], script_labels[s]) == 0) {
                        wc = g_colors.team[s];
                        break;
                    }
                }
                DrawText(TextFormat("%2d. %-18s", m + 1, match_winners[m]),
                         tx, ty, 20, wc);
            }

            int tally_x = sw - 260;
            int tally_y = cy;
            DrawText("--- WINS ---", tally_x, tally_y, 22, GRAY);
            tally_y += 30;
            for (int s = 0; s < TOTAL_SCRIPTS; s++) {
                if (win_count[s] == 0) continue;
                DrawText(TextFormat("%-18s %d", script_labels[s], win_count[s]),
                         tally_x, tally_y, 22, g_colors.team[s]);
                tally_y += 28;
            }
            if (timeouts > 0) {
                DrawText(TextFormat("%-18s %d", "timeout", timeouts),
                         tally_x, tally_y, 22, GRAY);
            }

            DrawText("Press ENTER / SPACE / ESC to continue",
                     cx - 200, sh - 40, 22, DARKGRAY);

            EndDrawing();
        }
    }

    if (gcfg.use_llm)
        llm_bot_shutdown();

    } /* end session loop */

    lighting_shutdown();
    fx_shutdown();
    CloseWindow();
    return 0;
}
