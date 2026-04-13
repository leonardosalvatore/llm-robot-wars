#pragma once
#include "raylib.h"

#define MAX_DYN_LIGHTS 8

void lighting_init(void);
void lighting_shutdown(void);

void lighting_update(float dt);

/* Upload light uniforms and activate the lit shader.
 * Call this, then draw terrain/walls, then call lighting_end(). */
void lighting_begin(Camera3D camera);
void lighting_end(void);

/* Spawn a temporary point light at the given world position. */
void lighting_add_explosion(float x, float z);
