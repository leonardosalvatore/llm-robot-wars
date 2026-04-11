#include "walls.h"
#include <math.h>

static Wall  g_walls[MAX_WALLS];
static int   g_wall_count = 0;

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
void walls_generate(float arena_half, int count, unsigned seed) {
    g_wall_count = 0;
    if (count <= 0) return;
    if (count > MAX_WALLS) count = MAX_WALLS;

    float margin  = arena_half * 0.12f;   /* keep walls away from edges */
    float inner   = arena_half - margin;
    float min_gap = 1.2f;                 /* minimum clear space between walls */

    unsigned s = seed;

    for (int i = 0; i < count; i++) {
        Wall w;
        w.height = 0.75f;

        bool ok = false;
        for (int try = 0; try < 30 && !ok; try++) {
            w.x = lcg_randf(&s, -inner, inner);
            w.z = lcg_randf(&s, -inner, inner);

            /* Alternate orientation; randomise length in [1.2, 3.5]
             * Thickness raised to 0.40 so fast projectiles cannot tunnel. */
            if ((i & 1) == 0) {
                w.hw = lcg_randf(&s, 1.2f, 3.5f);
                w.hd = 0.40f;
            } else {
                w.hw = 0.40f;
                w.hd = lcg_randf(&s, 1.2f, 3.5f);
            }

            /* Clamp so wall stays inside the arena */
            if (w.x - w.hw < -inner) w.x = -inner + w.hw;
            if (w.x + w.hw >  inner) w.x =  inner - w.hw;
            if (w.z - w.hd < -inner) w.z = -inner + w.hd;
            if (w.z + w.hd >  inner) w.z =  inner - w.hd;

            /* Reject if it overlaps an existing wall too closely */
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

        float ox = *px - w->x;
        float oz = *pz - w->z;
        float over_x = w->hw - fabsf(ox);
        float over_z = w->hd - fabsf(oz);

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
        if (px >= w->x - w->hw - margin && px <= w->x + w->hw + margin &&
            pz >= w->z - w->hd - margin && pz <= w->z + w->hd + margin)
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
    float h = 1.25f;   /* visible height  */

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
