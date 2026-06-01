# bot_llm.py — Live-edited by the LLM between matches.
#
# The LLM rewrites this file using the system prompt + the previous match
# telemetry. The COOKBOOK block at the BOTTOM of this file is reference
# material the LLM is asked to KEEP VERBATIM on every regeneration so that
# the next iteration always has a working set of Python patterns to copy.
#
# Active strategy:
#   wheels + flat body + 2x MachineGun (left/right) + 2x Laser (top_front/top_rear).
#   Chase the nearest enemy and hose it with everything; when self_hp drops
#   below 30% pick a random direction and keep firing while fleeing.

import math
import random


def init():
    return {
        "locomotion": "wheels",
        "body": "flat",
        "weapons": [
            {"type": "MachineGun", "mount": "left"},
            {"type": "MachineGun", "mount": "right"},
            {"type": "Laser", "mount": "top_front"},
            {"type": "Laser", "mount": "top_rear"},
        ],
    }


# IMPORTANT: module-level code runs once when the engine loads the script.
# At that moment self_x / self_z / self_team / self_hp do NOT exist yet.
# Never read them here. Seeding with random (no fixed seed) is fine.
LOW_HP_FRAC = 0.30
_panic_angle = random.uniform(0, math.pi * 2)
_panic_timer = 0.0


def _wall_avoid(targets):
    ax, az = 0.0, 0.0
    for r in targets:
        if r["type"] == "wall" and r["distance"] < 2.5:
            dx = self_x - r["x"]
            dz = self_z - r["z"]
            d = max(r["distance"], 0.05)
            ax += dx / (d * d)
            az += dz / (d * d)
    return ax, az


def _nearest_enemy(targets):
    best, best_d = None, float("inf")
    for t in targets:
        if t["type"] == "bot" and t["team"] != self_team and t["distance"] < best_d:
            best_d = t["distance"]
            best = t
    return best


def think(dt):
    global _panic_angle, _panic_timer

    _panic_timer -= dt

    targets = scan(0)
    ax, az = _wall_avoid(targets)
    enemy = _nearest_enemy(targets)

    if self_hp < self_max_hp * LOW_HP_FRAC:
        if _panic_timer <= 0.0:
            _panic_angle = random.uniform(0, math.pi * 2)
            _panic_timer = 0.6 + random.random() * 0.8
        move(math.cos(_panic_angle) + ax, math.sin(_panic_angle) + az)
        if enemy is not None and enemy["distance"] < 12.0:
            fire(enemy["x"] - self_x, enemy["z"] - self_z)
        return

    if enemy is not None:
        dx = enemy["x"] - self_x
        dz = enemy["z"] - self_z
        move(dx + ax, dz + az)
        fire(dx, dz)
    else:
        if _panic_timer <= 0.0:
            _panic_angle = random.uniform(0, math.pi * 2)
            _panic_timer = 1.0 + random.random()
        move(math.cos(_panic_angle) + ax, math.sin(_panic_angle) + az)


# ============================================================================
# EXAMPLES / COOKBOOK  —  KEEP THIS WHOLE COMMENT BLOCK VERBATIM ON EVERY
# REGENERATION. The LLM uses it as a working reference. Copy patterns from
# here into init()/think() above, but never delete the block itself.
# ============================================================================
#
# 1) init() returns a STATIC config dict. It never reads self_*; it runs before
#    self_x/self_z exist. Pick locomotion + body + weapons (1..4 entries, each
#    mount unique among "left"|"right"|"top"|"top_front"|"top_rear").
# ----------------------------------------------------------------------------
# def init():
#     return {
#         "locomotion": "wheels",
#         "body": "flat",
#         "weapons": [
#             {"type": "MachineGun", "mount": "left"},
#             {"type": "MachineGun", "mount": "right"},
#             {"type": "AutoCannon", "mount": "top_front"},
#             {"type": "AutoCannon", "mount": "top_rear"},
#         ],
#     }
#
# 2) Wall avoidance: returns a push-away vector to be added to your move().
# ----------------------------------------------------------------------------
# def _wall_avoid(targets):
#     ax, az = 0.0, 0.0
#     for r in targets:
#         if r["type"] == "wall" and r["distance"] < 2.5:
#             dx = self_x - r["x"]
#             dz = self_z - r["z"]
#             d = max(r["distance"], 0.05)
#             ax += dx / (d * d)
#             az += dz / (d * d)
#     return ax, az
#
# 3) Nearest-enemy picker. self_team is YOUR team; everyone else is fair game.
#    The LLM bot can see all teams, so compare with != self_team (NOT == 6).
# ----------------------------------------------------------------------------
# def _nearest_enemy(targets):
#     best, best_d = None, float("inf")
#     for t in targets:
#         if t["type"] == "bot" and t["team"] != self_team and t["distance"] < best_d:
#             best_d = t["distance"]
#             best = t
#     return best
#
# 4) A complete combat think(): chase, kite at low HP, fire when in range.
#    The engine rate-limits each weapon's fire interval, so calling fire()
#    every frame is safe and recommended. Just gate on having a target.
# ----------------------------------------------------------------------------
# def think(dt):
#     targets = scan(0)                       # radius is ignored
#     ax, az = _wall_avoid(targets)
#     enemy = _nearest_enemy(targets)
#     if enemy is not None:
#         dx = enemy["x"] - self_x
#         dz = enemy["z"] - self_z
#         if self_hp < self_max_hp * 0.25:
#             move(-dx + ax, -dz + az)        # kite away
#         else:
#             move(dx + ax, dz + az)          # close in
#         if enemy["distance"] < 14.0:
#             fire(dx, dz)                    # aims turret + fires ALL weapons
#     else:
#         move(ax + (random.random() - 0.5),
#              az + (random.random() - 0.5))
#
# 5) Range-tiered weapon selection with fire_weapon(i, dx, dz).
#    Indices are 0-based, in the order you listed weapons in init().
# ----------------------------------------------------------------------------
# if enemy["distance"] > 16.0:
#     fire_weapon(2, dx, dz)        # long range: lasers only (fast/accurate)
#     fire_weapon(3, dx, dz)
# elif enemy["distance"] < 5.0:
#     fire_weapon(0, dx, dz)        # close: brawler weapons only
#     fire_weapon(1, dx, dz)
# else:
#     fire(dx, dz)                  # mid range: everything
#
# 6) PITFALLS that the smoke test will reject (each one models have hit):
# ----------------------------------------------------------------------------
#    * This is PYTHON, not Lua. No 'local', no 'then', no 'end', no '--' comments.
#    * NEVER read self_x / self_z / self_team / self_hp at MODULE scope — they
#      do not exist until think() runs. Only read them inside init()/think().
#      (init() actually must NOT read them either; only think() should.)
#    * Module-level variables you ASSIGN inside think() need a 'global' line:
#          _t = 0.0
#          def think(dt):
#              global _t          # REQUIRED, else UnboundLocalError
#              _t += dt
#      If you assign a name anywhere in a function, Python treats it as local
#      for the WHOLE function — reading it before assignment then crashes.
#    * NEVER name a variable the same as an API function:
#          BAD : scan = scan(0)          # shadows the API; next call crashes
#          GOOD: targets = scan(0)
#    * scan() entries are dicts: use t["type"], t["x"], t["z"], t["distance"],
#      t["team"], t["hp"], t["max_hp"]. Wall entries only have type/x/z/distance.
#    * Use float("inf") for infinity, math.atan2(y, x) for angles (this is fine
#      in Python — unlike Lua), math.sqrt / math.hypot both exist in Python.
#    * fire_weapon() is 0-based: fire_weapon(0, dx, dz) fires the FIRST weapon.
#    * Smoke test runs 3.0 simulated seconds with 4 enemies at distance ~3.5u
#      front/back/left/right. If your think() never aims a fire() within 30
#      degrees of any of them, the script is rejected. Always fire toward enemies.
# ============================================================================
