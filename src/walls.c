#include "walls.h"
#include <math.h>

static Wall  g_walls[MAX_WALLS];
static int   g_wall_count = 0;

#define WALL_BOT_COLLISION_PAD 0.35f

/* -----------------------------------------------------------------------
 * Minimal LCG for reproducible wall placement, independent of rand().
 * ----------------------------------------------------------------------- */
static unsigned lcg_next(unsigned *s) {
    *s = *s * 1664525u + 1013904223u;
    return *s;
}

static float lcg_randf(unsigned *s, float lo, float hi) {
    float t = (float)(lcg_next(s) & 0x00FFFFFFu) / (float)0x01000000u;
    return lo + t * (hi - lo);
}

/* -----------------------------------------------------------------------
 * walls_generate
 *
 * Places `count` walls with alternating horizontal / vertical orientation.
 * Walls are kept inside the arena with a small edge margin.  A simple
 * overlap rejection prevents walls from stacking directly on top of each
 * other (up to 20 retry attempts per wall).
 * ----------------------------------------------------------------------- */
static float snap_grid(float v, float step) {
    return floorf(v / step + 0.5f) * step;
}

void walls_generate(float arena_half_x, float arena_half_z,
                    int count, int wall_size, unsigned seed)
{
    g_wall_count = 0;
    if (count <= 0) return;
    if (count > MAX_WALLS) count = MAX_WALLS;

    const float GRID = 2.0f;

    float margin_x = arena_half_x * 0.12f;
    float margin_z = arena_half_z * 0.12f;
    float inner_x  = arena_half_x - margin_x;
    float inner_z  = arena_half_z - margin_z;
    float min_gap  = 1.2f;

    if (wall_size < 1) wall_size = 1;
    if (wall_size > 5) wall_size = 5;

    unsigned s = seed;

    for (int i = 0; i < count; i++) {
        Wall w;
        w.height = 0.525f;

        bool ok = false;
        for (int try = 0; try < 30 && !ok; try++) {
            w.x = snap_grid(lcg_randf(&s, -inner_x, inner_x), GRID);
            w.z = snap_grid(lcg_randf(&s, -inner_z, inner_z), GRID);

            if (wall_size <= 1) {
                /* Thin line walls (original behaviour) */
                if ((i & 1) == 0) {
                    w.hw = snap_grid(lcg_randf(&s, 1.2f, 3.5f), GRID * 0.5f);
                    w.hd = 0.40f;
                } else {
                    w.hw = 0.40f;
                    w.hd = snap_grid(lcg_randf(&s, 1.2f, 3.5f), GRID * 0.5f);
                }
            } else {
                /* Rectangular walls — both axes get real extent */
                float base = (float)wall_size * GRID * 0.5f;
                if ((i & 1) == 0) {
                    w.hw = snap_grid(lcg_randf(&s, base * 0.5f, base), GRID * 0.5f);
                    w.hd = snap_grid(lcg_randf(&s, base * 0.3f, base * 0.7f), GRID * 0.5f);
                } else {
                    w.hw = snap_grid(lcg_randf(&s, base * 0.3f, base * 0.7f), GRID * 0.5f);
                    w.hd = snap_grid(lcg_randf(&s, base * 0.5f, base), GRID * 0.5f);
                }
                if (w.hw < GRID * 0.5f) w.hw = GRID * 0.5f;
                if (w.hd < GRID * 0.5f) w.hd = GRID * 0.5f;
            }

            if (w.x - w.hw < -inner_x) w.x = -inner_x + w.hw;
            if (w.x + w.hw >  inner_x) w.x =  inner_x - w.hw;
            if (w.z - w.hd < -inner_z) w.z = -inner_z + w.hd;
            if (w.z + w.hd >  inner_z) w.z =  inner_z - w.hd;

            ok = true;
            for (int j = 0; j < g_wall_count; j++) {
                float dx = fabsf(w.x - g_walls[j].x);
                float dz = fabsf(w.z - g_walls[j].z);
                float need_x = w.hw + g_walls[j].hw + min_gap;
                float need_z = w.hd + g_walls[j].hd + min_gap;
                if (dx < need_x && dz < need_z) { ok = false; break; }
            }
        }

        if (ok) g_walls[g_wall_count++] = w;
    }
}

/* -----------------------------------------------------------------------
 * Accessors
 * ----------------------------------------------------------------------- */
int         walls_count(void)    { return g_wall_count; }
const Wall *walls_get(void)      { return g_walls;      }

/* -----------------------------------------------------------------------
 * walls_block_segment — Liang-Barsky slab test (2-D, XZ plane).
 *
 * Parameterises the segment as P(t) = (x0,z0) + t*(dx,dz), t∈[0,1].
 * Clips t against the X and Z slabs of each wall's AABB.
 * ----------------------------------------------------------------------- */
bool walls_block_segment(float x0, float z0, float x1, float z1) {
    float dx = x1 - x0;
    float dz = z1 - z0;

    for (int i = 0; i < g_wall_count; i++) {
        const Wall *w = &g_walls[i];
        float t_min = 0.0f, t_max = 1.0f;

        /* X slab */
        if (fabsf(dx) < 1e-6f) {
            if (x0 < w->x - w->hw || x0 > w->x + w->hw) continue;
        } else {
            float t1 = (w->x - w->hw - x0) / dx;
            float t2 = (w->x + w->hw - x0) / dx;
            if (t1 > t2) { float tmp = t1; t1 = t2; t2 = tmp; }
            if (t1 > t_min) t_min = t1;
            if (t2 < t_max) t_max = t2;
            if (t_min > t_max) continue;
        }

        /* Z slab */
        if (fabsf(dz) < 1e-6f) {
            if (z0 < w->z - w->hd || z0 > w->z + w->hd) continue;
        } else {
            float t1 = (w->z - w->hd - z0) / dz;
            float t2 = (w->z + w->hd - z0) / dz;
            if (t1 > t2) { float tmp = t1; t1 = t2; t2 = tmp; }
            if (t1 > t_min) t_min = t1;
            if (t2 < t_max) t_max = t2;
            if (t_min > t_max) continue;
        }

        return true;
    }
    return false;
}

/* -----------------------------------------------------------------------
 * walls_point_inside — fast AABB containment test for projectiles.
 * ----------------------------------------------------------------------- */
bool walls_point_inside(float px, float pz) {
    for (int i = 0; i < g_wall_count; i++) {
        const Wall *w = &g_walls[i];
        if (px >= w->x - w->hw && px <= w->x + w->hw &&
            pz >= w->z - w->hd && pz <= w->z + w->hd)
            return true;
    }
    return false;
}

/* -----------------------------------------------------------------------
 * walls_push_out_bot — resolve bot overlap with a wall using the
 * minimum-penetration axis, then zero the velocity on that axis so the
 * bot doesn't slide into the wall again next frame.
 * ----------------------------------------------------------------------- */
void walls_push_out_bot(float *px, float *pz, float *vx, float *vz) {
    for (int i = 0; i < g_wall_count; i++) {
        const Wall *w = &g_walls[i];
        float hw = w->hw + WALL_BOT_COLLISION_PAD;
        float hd = w->hd + WALL_BOT_COLLISION_PAD;

        float ox = *px - w->x;
        float oz = *pz - w->z;
        float over_x = hw - fabsf(ox);
        float over_z = hd - fabsf(oz);

        if (over_x <= 0.0f || over_z <= 0.0f) continue; /* no overlap */

        /* Push out along the shallower axis */
        if (over_x < over_z) {
            *px += (ox >= 0.0f) ? over_x : -over_x;
            if ((ox >= 0.0f && *vx < 0.0f) || (ox < 0.0f && *vx > 0.0f))
                *vx = 0.0f;
        } else {
            *pz += (oz >= 0.0f) ? over_z : -over_z;
            if ((oz >= 0.0f && *vz < 0.0f) || (oz < 0.0f && *vz > 0.0f))
                *vz = 0.0f;
        }
    }
}

/* -----------------------------------------------------------------------
 * walls_safe_spawn — returns true when (px, pz) is outside every wall AABB
 * expanded by `margin` on all sides.
 * ----------------------------------------------------------------------- */
bool walls_safe_spawn(float px, float pz, float margin) {
    for (int i = 0; i < g_wall_count; i++) {
        const Wall *w = &g_walls[i];
        float expand = margin + WALL_BOT_COLLISION_PAD;
        if (px >= w->x - w->hw - expand && px <= w->x + w->hw + expand &&
            pz >= w->z - w->hd - expand && pz <= w->z + w->hd + expand)
            return false;
    }
    return true;
}

/* -----------------------------------------------------------------------
 * walls_add_border — append four solid perimeter walls around the arena.
 * The inner face of each wall is flush with ±ahx / ±ahz.
 * Thickness is 0.5 units; height 2.5 so they are clearly visible above bots.
 * ----------------------------------------------------------------------- */
void walls_add_border(float ahx, float ahz) {
    if (g_wall_count + 4 > MAX_WALLS) return;  /* shouldn't happen */

    float t = 0.25f;   /* half-thickness */
    float h = 0.525f;  /* same height as generated walls */

    /* North face (z = -ahz) */
    g_walls[g_wall_count++] = (Wall){ .x =  0.0f,    .z = -(ahz + t),
                                      .hw = ahx + t,  .hd = t, .height = h };
    /* South face (z = +ahz) */
    g_walls[g_wall_count++] = (Wall){ .x =  0.0f,    .z = +(ahz + t),
                                      .hw = ahx + t,  .hd = t, .height = h };
    /* West face (x = -ahx) — extend Z to cover corners */
    g_walls[g_wall_count++] = (Wall){ .x = -(ahx + t), .z = 0.0f,
                                      .hw = t,  .hd = ahz + t, .height = h };
    /* East face (x = +ahx) — extend Z to cover corners */
    g_walls[g_wall_count++] = (Wall){ .x = +(ahx + t), .z = 0.0f,
                                      .hw = t,  .hd = ahz + t, .height = h };
}
