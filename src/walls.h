#pragma once
#include <stdbool.h>

typedef struct {
    float x, z;
    float hw, hd;
    float height;
} Wall;

#define MAX_WALLS 72

void        walls_generate(float arena_half_x, float arena_half_z,
                           int count, int wall_size, unsigned seed);
void        walls_add_border(float arena_half_x, float arena_half_z);
int         walls_count(void);
const Wall *walls_get(void);
bool        walls_block_segment(float x0, float z0, float x1, float z1);
bool        walls_point_inside(float px, float pz);
void        walls_push_out_bot(float *px, float *pz, float *vx, float *vz);
bool        walls_safe_spawn(float px, float pz, float margin);
