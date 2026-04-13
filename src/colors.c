#include "colors.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ----------------------------------------------------------------------- */

typedef struct {
    const char *name;
    size_t      offset;
    const char *comment;
} ColorEntry;

#define CE(field, cmt) { #field, offsetof(GameColors, field), cmt }

static const ColorEntry entries[] = {
    CE(terrain,         "arena floor"),
    CE(grid,            "grid lines"),
    CE(wall_fill,       "interior wall faces"),
    CE(wall_wire,       "interior wall wireframe"),
    CE(border_fill,     "border wall faces"),
    CE(border_wire,     "border wall wireframe"),
    CE(bot_tread,       "tank tread"),
    CE(bot_weapon,      "mounted weapons"),
    CE(hp_full,         "HP bar full"),
    CE(hp_mid,          "HP bar mid"),
    CE(hp_low,          "HP bar low"),
    CE(hp_bg,           "HP / armour bar background"),
    CE(armour_fill,     "armour bar fill"),
    CE(armour_bg,       "armour bar background"),
    CE(laser,           "laser projectile"),
    CE(explosion_ring,  "explosion shockwave"),
    CE(explosion_flash, "explosion flash"),
    CE(bg_clear,        "main game clear"),
    CE(bg_config,       "config screen"),
    CE(bg_results,      "results screen"),
};
#define N_ENTRIES ((int)(sizeof(entries)/sizeof(entries[0])))

#undef CE

/* ----------------------------------------------------------------------- */

static Color *field_ptr(GameColors *c, size_t off) {
    return (Color *)((char *)c + off);
}

static int parse_rgba(const char *s, Color *out) {
    int r, g, b, a = 255;
    int n = sscanf(s, "%d , %d , %d , %d", &r, &g, &b, &a);
    if (n < 3) return 0;
    out->r = (unsigned char)r;
    out->g = (unsigned char)g;
    out->b = (unsigned char)b;
    out->a = (unsigned char)a;
    return 1;
}

/* ----------------------------------------------------------------------- */

void colors_set_defaults(GameColors *c) {
    c->terrain      = (Color){80, 80, 80, 255};
    c->grid         = (Color){30, 30, 30, 255};

    c->wall_fill    = (Color){120, 110, 100, 255};
    c->wall_wire    = (Color){80,  75,  70, 255};
    c->border_fill  = (Color){60,  65,  80, 255};
    c->border_wire  = (Color){40,  45,  60, 255};

    c->team[0] = (Color){130,255,130,255};
    c->team[1] = (Color){100,230,100,255};
    c->team[2] = (Color){ 80,210, 80,255};
    c->team[3] = (Color){ 60,190, 60,255};
    c->team[4] = (Color){ 50,170, 50,255};
    c->team[5] = (Color){ 40,155, 40,255};
    c->team[6] = (Color){ 30,230,255,255};

    c->bot_tread    = (Color){75, 75, 75, 255};
    c->bot_weapon   = (Color){130,130,130,255};

    c->hp_full      = (Color){  0,228, 48,255};
    c->hp_mid       = (Color){253,249,  0,255};
    c->hp_low       = (Color){230, 41, 55,255};
    c->hp_bg        = (Color){ 20, 20, 20,200};
    c->armour_fill  = (Color){180,180,255,255};
    c->armour_bg    = (Color){ 20, 20, 20,200};

    c->laser        = (Color){230, 41, 55,255};

    c->explosion[0] = (Color){255, 70, 10,255};
    c->explosion[1] = (Color){255,140, 30,255};
    c->explosion[2] = (Color){255,220, 60,255};
    c->explosion[3] = (Color){200, 40,  5,255};
    c->explosion[4] = (Color){255,255, 90,255};
    c->explosion_ring  = (Color){255,140, 30,200};
    c->explosion_flash = (Color){255,255,220,255};

    c->bg_clear     = (Color){  0,  0,  0,255};
    c->bg_config    = (Color){ 20, 20, 30,255};
    c->bg_results   = (Color){ 10, 15, 25,255};
}

/* ----------------------------------------------------------------------- */

void colors_load(GameColors *c, const char *path) {
    colors_set_defaults(c);
    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';

        char key[64] = {0};
        char val[128] = {0};
        if (sscanf(line, " %63[a-z_0-9] = %127[^\n]", key, val) != 2) continue;

        /* Trim trailing whitespace from val */
        int len = (int)strlen(val);
        while (len > 0 && (val[len-1] == ' ' || val[len-1] == '\t' || val[len-1] == '\r'))
            val[--len] = '\0';

        /* Check team_N and explosion_N first */
        if (strncmp(key, "team_", 5) == 0) {
            int idx = atoi(key + 5);
            if (idx >= 0 && idx < TOTAL_SCRIPTS) parse_rgba(val, &c->team[idx]);
            continue;
        }
        if (strncmp(key, "explosion_", 10) == 0 && key[10] >= '0' && key[10] <= '4') {
            int idx = key[10] - '0';
            parse_rgba(val, &c->explosion[idx]);
            continue;
        }

        for (int i = 0; i < N_ENTRIES; i++) {
            if (strcmp(key, entries[i].name) == 0) {
                parse_rgba(val, field_ptr(c, entries[i].offset));
                break;
            }
        }
    }
    fclose(f);
}

/* ----------------------------------------------------------------------- */

static void write_color(FILE *f, const char *name, Color c, const char *cmt) {
    if (c.a == 255)
        fprintf(f, "%-20s = %3d, %3d, %3d", name, c.r, c.g, c.b);
    else
        fprintf(f, "%-20s = %3d, %3d, %3d, %3d", name, c.r, c.g, c.b, c.a);
    if (cmt) fprintf(f, "       # %s", cmt);
    fprintf(f, "\n");
}

void colors_save(const GameColors *c, const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return;

    fprintf(f, "# Terrain\n");
    write_color(f, "terrain",     c->terrain,     "arena floor");
    write_color(f, "grid",        c->grid,        "grid lines");
    fprintf(f, "\n# Walls\n");
    write_color(f, "wall_fill",   c->wall_fill,   "interior wall faces");
    write_color(f, "wall_wire",   c->wall_wire,   "interior wall wireframe");
    write_color(f, "border_fill", c->border_fill, "border wall faces");
    write_color(f, "border_wire", c->border_wire, "border wall wireframe");

    fprintf(f, "\n# Team colors (0-5 = non-LLM, 6 = LLM)\n");
    static const char *team_cmt[] = {
        "bot_light", "bot_skirmisher", "bot_chaser",
        "bot_duelist", "bot_lancer", "bot_fortress", "bot_llm"
    };
    for (int i = 0; i < TOTAL_SCRIPTS; i++) {
        char name[16];
        snprintf(name, sizeof(name), "team_%d", i);
        write_color(f, name, c->team[i], team_cmt[i]);
    }

    fprintf(f, "\n# Bot parts\n");
    write_color(f, "bot_tread",   c->bot_tread,   "tank tread");
    write_color(f, "bot_weapon",  c->bot_weapon,  "mounted weapons");

    fprintf(f, "\n# HP / Armour bars\n");
    write_color(f, "hp_full",     c->hp_full,     "full health");
    write_color(f, "hp_mid",      c->hp_mid,      "mid health");
    write_color(f, "hp_low",      c->hp_low,      "low health");
    write_color(f, "hp_bg",       c->hp_bg,       "bar background");
    write_color(f, "armour_fill", c->armour_fill,  "armour fill");
    write_color(f, "armour_bg",   c->armour_bg,    "armour background");

    fprintf(f, "\n# Projectiles\n");
    write_color(f, "laser",       c->laser,       "laser beam");

    fprintf(f, "\n# Explosions\n");
    for (int i = 0; i < 5; i++) {
        char name[16];
        snprintf(name, sizeof(name), "explosion_%d", i);
        write_color(f, name, c->explosion[i], NULL);
    }
    write_color(f, "explosion_ring",  c->explosion_ring,  "shockwave ring");
    write_color(f, "explosion_flash", c->explosion_flash, "central flash");

    fprintf(f, "\n# Backgrounds\n");
    write_color(f, "bg_clear",    c->bg_clear,    "main game clear");
    write_color(f, "bg_config",   c->bg_config,   "config screen");
    write_color(f, "bg_results",  c->bg_results,  "results screen");

    fclose(f);
}
