#pragma once
#include "raylib.h"
#include "game.h"

typedef struct {
    Color terrain;
    Color grid;

    Color wall_fill;
    Color wall_wire;
    Color border_fill;
    Color border_wire;

    Color team[TOTAL_SCRIPTS];

    Color bot_tread;
    Color bot_weapon;

    Color hp_full;
    Color hp_mid;
    Color hp_low;
    Color hp_bg;
    Color armour_fill;
    Color armour_bg;

    Color laser;

    Color scan_wall;
    Color scan_enemy;

    Color explosion[5];
    Color explosion_ring;
    Color explosion_flash;

    Color bg_clear;
    Color bg_config;
    Color bg_results;
} GameColors;

extern GameColors g_colors;

void colors_set_defaults(GameColors *c);
void colors_load(GameColors *c, const char *path);
void colors_save(const GameColors *c, const char *path);
