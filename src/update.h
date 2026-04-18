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

typedef struct {
    int   think_frames;
    int   enemy_visible_frames;
    int   fire_frames;
    int   shots_fired;
    int   shots_hit;
    int   arena_bumps;
    int   wall_bumps;
    float nearest_dist_sum;
    int   nearest_dist_samples;
} LlmTelemetry;

void update_telemetry_reset(void);
void update_telemetry_get(LlmTelemetry *out);
void update_telemetry_inc_fire_frame(void);
void update_telemetry_inc_shots_fired(int n);
void update_telemetry_inc_shots_hit(void);
void update_telemetry_inc_think_frame(bool enemy_visible, float nearest_dist);
void update_telemetry_inc_arena_bump(void);
void update_telemetry_inc_wall_bump(void);
