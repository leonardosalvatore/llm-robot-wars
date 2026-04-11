#pragma once

#include "game.h"

void update_set_arena(float half_x, float half_z);

void update_scripts(Bot *bots, int count, float dt);
void update_inertia(Bot *bots, int count, float dt);
void update_movement(Bot *bots, int count, float dt);
void update_projectiles(Proj *projs, int *pcount, Bot *bots, int bcount, float dt);

void update_reset_llm_stats(void);
void update_get_llm_stats(float *dmg_out, int *kills_out);

void update_clear_runtime_error(void);
void update_get_runtime_error(char *buf, int size);
