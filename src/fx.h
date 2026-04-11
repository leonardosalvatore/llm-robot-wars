#pragma once
#include "raylib.h"

/* Initialise/shutdown the particle system and glow shader. */
void fx_init(void);
void fx_shutdown(void);

/* Advance all live particles and rings by `dt` seconds. */
void fx_update(float dt);

/* Draw all live particles and rings.
 * Must be called inside BeginMode3D / EndMode3D. */
void fx_draw(void);

/* Spawn a small spark burst at world position (x, y, z).
 * `col` tints the sparks with the projectile's team colour. */
void fx_impact(float x, float y, float z, Color col);

/* Spawn a full explosion at (x, z) — used when a bot is destroyed. */
void fx_explosion(float x, float z);
