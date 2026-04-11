#include "fx.h"
#include "rlgl.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* -----------------------------------------------------------------------
 * Capacities
 * ----------------------------------------------------------------------- */
#define FX_MAX_PARTICLES 512
#define FX_MAX_RINGS      64

/* -----------------------------------------------------------------------
 * Data
 * ----------------------------------------------------------------------- */
typedef struct {
    Vector3 pos;
    Vector3 vel;
    Color   color;
    float   life;
    float   max_life;
    float   size;       /* radius at full life */
    int     active;
} FxParticle;

typedef struct {
    Vector3 center;
    Color   color;
    float   radius;
    float   expand_rate;  /* units per second */
    float   life;
    float   max_life;
    int     active;
} FxRing;

static FxParticle g_particles[FX_MAX_PARTICLES];
static FxRing     g_rings[FX_MAX_RINGS];

/* -----------------------------------------------------------------------
 * Glow shader (inline — no external files needed)
 * Vertex: standard Raylib passthrough.
 * Fragment: boosts brightness 1.6× so additive blending creates a halo.
 * ----------------------------------------------------------------------- */
static Shader g_shader;
static int    g_shader_ok = 0;

static const char *GLOW_VS =
    "#version 330\n"
    "layout(location=0) in vec3 vertexPosition;\n"
    "layout(location=1) in vec2 vertexTexCoord;\n"
    "layout(location=3) in vec4 vertexColor;\n"
    "uniform mat4 mvp;\n"
    "out vec4 fragColor;\n"
    "void main() {\n"
    "    fragColor    = vertexColor;\n"
    "    gl_Position  = mvp * vec4(vertexPosition, 1.0);\n"
    "}\n";

static const char *GLOW_FS =
    "#version 330\n"
    "in  vec4 fragColor;\n"
    "out vec4 finalColor;\n"
    "void main() {\n"
    "    finalColor = vec4(fragColor.rgb * 1.6, fragColor.a);\n"
    "}\n";

/* -----------------------------------------------------------------------
 * Helpers
 * ----------------------------------------------------------------------- */
static float randf(float lo, float hi) {
    return lo + ((float)rand() / (float)RAND_MAX) * (hi - lo);
}

static FxParticle *alloc_particle(void) {
    for (int i = 0; i < FX_MAX_PARTICLES; i++)
        if (!g_particles[i].active) return &g_particles[i];
    return NULL;
}

static FxRing *alloc_ring(void) {
    for (int i = 0; i < FX_MAX_RINGS; i++)
        if (!g_rings[i].active) return &g_rings[i];
    return NULL;
}

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */
void fx_init(void) {
    memset(g_particles, 0, sizeof(g_particles));
    memset(g_rings,     0, sizeof(g_rings));
    g_shader    = LoadShaderFromMemory(GLOW_VS, GLOW_FS);
    g_shader_ok = (g_shader.id > 0);
}

void fx_shutdown(void) {
    if (g_shader_ok) {
        UnloadShader(g_shader);
        g_shader_ok = 0;
    }
}

void fx_update(float dt) {
    for (int i = 0; i < FX_MAX_PARTICLES; i++) {
        FxParticle *p = &g_particles[i];
        if (!p->active) continue;
        p->life -= dt;
        if (p->life <= 0.0f) { p->active = 0; continue; }
        p->pos.x += p->vel.x * dt;
        p->pos.y += p->vel.y * dt;
        p->pos.z += p->vel.z * dt;
        p->vel.y -= 5.0f * dt;  /* gravity */
        if (p->pos.y < 0.02f) { p->pos.y = 0.02f; p->vel.y *= -0.25f; }
    }
    for (int i = 0; i < FX_MAX_RINGS; i++) {
        FxRing *r = &g_rings[i];
        if (!r->active) continue;
        r->life -= dt;
        if (r->life <= 0.0f) { r->active = 0; continue; }
        r->radius += r->expand_rate * dt;
    }
}

void fx_draw(void) {
    if (g_shader_ok) BeginShaderMode(g_shader);
    BeginBlendMode(BLEND_ADDITIVE);

    /* Particles — tiny spheres that shrink and fade as life runs out */
    for (int i = 0; i < FX_MAX_PARTICLES; i++) {
        const FxParticle *p = &g_particles[i];
        if (!p->active) continue;
        float t   = p->life / p->max_life;          /* 1→0 */
        float rad = p->size * (0.3f + 0.7f * t);    /* shrinks toward end */
        Color c   = p->color;
        c.a = (unsigned char)(c.a * t);
        DrawSphereEx(p->pos, rad, 4, 4, c);
    }

    /* Rings — flat ground circles that expand and fade */
    for (int i = 0; i < FX_MAX_RINGS; i++) {
        const FxRing *r = &g_rings[i];
        if (!r->active) continue;
        float t = r->life / r->max_life;
        Color c = r->color;
        c.a = (unsigned char)(c.a * t);
        DrawCircle3D(r->center, r->radius, (Vector3){1,0,0}, 90.0f, c);
    }

    EndBlendMode();
    if (g_shader_ok) EndShaderMode();
}

/* -----------------------------------------------------------------------
 * Impact — projectile hits a bot
 * ----------------------------------------------------------------------- */
void fx_impact(float x, float y, float z, Color col) {
    /* 8 sparks that fly outward and upward */
    for (int i = 0; i < 8; i++) {
        FxParticle *p = alloc_particle();
        if (!p) break;
        float angle = randf(0, 2.0f * 3.14159265f);
        float speed = randf(1.2f, 3.5f);
        p->pos     = (Vector3){x, y, z};
        p->vel     = (Vector3){cosf(angle)*speed, randf(0.4f,1.8f), sinf(angle)*speed};
        p->color   = (Color){
            (unsigned char)((col.r + 255) / 2),
            (unsigned char)((col.g + 200) / 2),
            50, 255 };
        p->max_life = randf(0.18f, 0.35f);
        p->life     = p->max_life;
        p->size     = randf(0.025f, 0.055f);
        p->active   = 1;
    }

    /* Small expanding ring at ground level */
    FxRing *r = alloc_ring();
    if (r) {
        r->center      = (Vector3){x, 0.04f, z};
        r->color       = (Color){col.r, col.g, col.b, 220};
        r->radius      = 0.04f;
        r->expand_rate = 2.5f;
        r->max_life    = 0.14f;
        r->life        = r->max_life;
        r->active      = 1;
    }
}

/* -----------------------------------------------------------------------
 * Explosion — bot is destroyed
 * ----------------------------------------------------------------------- */
void fx_explosion(float x, float z) {
    static const Color palette[] = {
        {255,  70, 10, 255}, {255, 140, 30, 255},
        {255, 220, 60, 255}, {200,  40,  5, 255},
        {255, 255, 90, 255},
    };
    float y0 = 0.25f;

    /* 22 debris particles */
    for (int i = 0; i < 22; i++) {
        FxParticle *p = alloc_particle();
        if (!p) break;
        float angle = randf(0, 2.0f * 3.14159265f);
        float speed = randf(1.5f, 6.5f);
        p->pos     = (Vector3){x, y0, z};
        p->vel     = (Vector3){cosf(angle)*speed, randf(1.2f,5.0f), sinf(angle)*speed};
        p->color   = palette[rand() % 5];
        p->max_life = randf(0.45f, 0.90f);
        p->life     = p->max_life;
        p->size     = randf(0.04f, 0.11f);
        p->active   = 1;
    }

    /* Shockwave ring */
    FxRing *r = alloc_ring();
    if (r) {
        r->center      = (Vector3){x, 0.04f, z};
        r->color       = (Color){255, 140, 30, 200};
        r->radius      = 0.15f;
        r->expand_rate = 3.8f;
        r->max_life    = 0.38f;
        r->life        = r->max_life;
        r->active      = 1;
    }

    /* Central bright flash */
    FxParticle *flash = alloc_particle();
    if (flash) {
        flash->pos     = (Vector3){x, y0, z};
        flash->vel     = (Vector3){0, 0, 0};
        flash->color   = (Color){255, 255, 220, 255};
        flash->max_life = 0.10f;
        flash->life    = flash->max_life;
        flash->size    = 0.30f;
        flash->active  = 1;
    }
}
