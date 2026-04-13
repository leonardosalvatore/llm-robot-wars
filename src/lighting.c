#include "lighting.h"
#include "rlgl.h"
#include <string.h>
#include <stdio.h>

/* -----------------------------------------------------------------------
 * Inline shaders — no external files needed.
 *
 * The vertex shader passes world-space position and normal straight through
 * because terrain and walls are drawn without rlPushMatrix (their vertex
 * positions are already in world space).
 *
 * The fragment shader adds up to MAX_DYN_LIGHTS point lights with
 * quadratic distance fall-off and a half-Lambert diffuse term so the
 * floor directly under an explosion still catches light.
 * ----------------------------------------------------------------------- */

static const char *LIT_VS =
    "#version 330\n"
    "layout(location=0) in vec3 vertexPosition;\n"
    "layout(location=1) in vec2 vertexTexCoord;\n"
    "layout(location=2) in vec3 vertexNormal;\n"
    "layout(location=3) in vec4 vertexColor;\n"
    "uniform mat4 mvp;\n"
    "out vec3 fragWorldPos;\n"
    "out vec3 fragNormal;\n"
    "out vec4 fragColor;\n"
    "void main() {\n"
    "    fragWorldPos = vertexPosition;\n"
    "    fragNormal   = vertexNormal;\n"
    "    fragColor    = vertexColor;\n"
    "    gl_Position  = mvp * vec4(vertexPosition, 1.0);\n"
    "}\n";

static const char *LIT_FS =
    "#version 330\n"
    "in vec3 fragWorldPos;\n"
    "in vec3 fragNormal;\n"
    "in vec4 fragColor;\n"
    "uniform vec4 colDiffuse;\n"
    "uniform vec3 ambient;\n"
    "#define MAX_DYN 8\n"
    "uniform int   dynOn[MAX_DYN];\n"
    "uniform vec3  dynPos[MAX_DYN];\n"
    "uniform vec3  dynCol[MAX_DYN];\n"
    "uniform float dynRad[MAX_DYN];\n"
    "out vec4 finalColor;\n"
    "void main() {\n"
    "    vec3 base = fragColor.rgb * colDiffuse.rgb;\n"
    "    vec3 n    = normalize(fragNormal);\n"
    "    vec3 lit  = ambient;\n"
    "    for (int i = 0; i < MAX_DYN; i++) {\n"
    "        if (dynOn[i] != 0) {\n"
    "            vec3  toL  = dynPos[i] - fragWorldPos;\n"
    "            float dist = length(toL);\n"
    "            float att  = clamp(1.0 - dist / dynRad[i], 0.0, 1.0);\n"
    "            att *= att;\n"
    "            float NdL  = max(dot(n, normalize(toL)), 0.0);\n"
    "            lit += dynCol[i] * att * (0.4 + 0.6 * NdL);\n"
    "        }\n"
    "    }\n"
    "    finalColor = vec4(base * lit, fragColor.a * colDiffuse.a);\n"
    "}\n";

/* ----------------------------------------------------------------------- */

typedef struct {
    Vector3 position;
    Color   color;
    float   radius;
    float   life;
    float   max_life;
    int     active;
} DynLight;

static Shader   g_lit_shader;
static int      g_lit_ok;

static int loc_ambient;
static int loc_on [MAX_DYN_LIGHTS];
static int loc_pos[MAX_DYN_LIGHTS];
static int loc_col[MAX_DYN_LIGHTS];
static int loc_rad[MAX_DYN_LIGHTS];

static DynLight g_lights[MAX_DYN_LIGHTS];

/* ----------------------------------------------------------------------- */

void lighting_init(void) {
    memset(g_lights, 0, sizeof(g_lights));

    g_lit_shader = LoadShaderFromMemory(LIT_VS, LIT_FS);
    g_lit_ok = (g_lit_shader.id > 0);
    if (!g_lit_ok) {
        fprintf(stderr, "[lighting] failed to compile lit shader\n");
        return;
    }

    loc_ambient = GetShaderLocation(g_lit_shader, "ambient");
    for (int i = 0; i < MAX_DYN_LIGHTS; i++) {
        loc_on [i] = GetShaderLocation(g_lit_shader, TextFormat("dynOn[%i]",  i));
        loc_pos[i] = GetShaderLocation(g_lit_shader, TextFormat("dynPos[%i]", i));
        loc_col[i] = GetShaderLocation(g_lit_shader, TextFormat("dynCol[%i]", i));
        loc_rad[i] = GetShaderLocation(g_lit_shader, TextFormat("dynRad[%i]", i));
    }
}

void lighting_shutdown(void) {
    if (g_lit_ok) {
        UnloadShader(g_lit_shader);
        g_lit_ok = 0;
    }
}

/* ----------------------------------------------------------------------- */

void lighting_update(float dt) {
    for (int i = 0; i < MAX_DYN_LIGHTS; i++) {
        DynLight *l = &g_lights[i];
        if (!l->active) continue;
        l->life -= dt;
        if (l->life <= 0.0f) l->active = 0;
    }
}

/* ----------------------------------------------------------------------- */

void lighting_begin(Camera3D camera) {
    if (!g_lit_ok) return;

    float amb[3] = {0.85f, 0.85f, 0.90f};
    SetShaderValue(g_lit_shader, loc_ambient, amb, SHADER_UNIFORM_VEC3);

    for (int i = 0; i < MAX_DYN_LIGHTS; i++) {
        DynLight *l = &g_lights[i];
        int on = l->active;
        SetShaderValue(g_lit_shader, loc_on[i], &on, SHADER_UNIFORM_INT);

        if (l->active) {
            float t = l->life / l->max_life;
            float pos[3] = {l->position.x, l->position.y, l->position.z};
            float col[3] = {
                (l->color.r / 255.0f) * t,
                (l->color.g / 255.0f) * t,
                (l->color.b / 255.0f) * t
            };
            float rad = l->radius * (0.3f + 0.7f * t);
            SetShaderValue(g_lit_shader, loc_pos[i], pos, SHADER_UNIFORM_VEC3);
            SetShaderValue(g_lit_shader, loc_col[i], col, SHADER_UNIFORM_VEC3);
            SetShaderValue(g_lit_shader, loc_rad[i], &rad, SHADER_UNIFORM_FLOAT);
        }
    }

    (void)camera;
    BeginShaderMode(g_lit_shader);
}

void lighting_end(void) {
    if (!g_lit_ok) return;
    EndShaderMode();
}

/* ----------------------------------------------------------------------- */

void lighting_add_explosion(float x, float z) {
    int best = -1;
    float min_life = 1e9f;
    for (int i = 0; i < MAX_DYN_LIGHTS; i++) {
        if (!g_lights[i].active) { best = i; break; }
        if (g_lights[i].life < min_life) {
            min_life = g_lights[i].life;
            best = i;
        }
    }
    if (best < 0) best = 0;

    DynLight *l = &g_lights[best];
    l->position = (Vector3){x, 3.0f, z};
    l->color    = (Color){255, 180, 50, 255};
    l->radius   = 15.0f;
    l->max_life = 0.7f;
    l->life     = l->max_life;
    l->active   = 1;
}
