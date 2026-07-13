# bot_llm.py — Live-edited by the LLM between matches.
#
# The LLM rewrites this file using the system prompt + the previous match
# telemetry. The COOKBOOK block at the BOTTOM of this file is reference
# material the LLM is asked to KEEP VERBATIM on every regeneration so that
# the next iteration always has a working set of Python patterns to copy.
#
# Active strategy (coordinated):
#   wheels + flat body + 2x MachineGun (left/right) + 2x Laser (top_front/top_rear).
#   Bots steer toward the arena centre (never hug the border), keep spacing from
#   teammates, and CONCENTRATE FIRE on one shared enemy chosen via team_mem.
#   Even self_id push in (attack); odd self_id kite at range (defence). When
#   self_hp drops below 30% they fall back toward centre and keep firing.

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


def _steer_to_center():
    # Pull toward (0,0), stronger the closer we are to the arena border.
    hx = max(arena_half_x, 1.0)
    hz = max(arena_half_z, 1.0)
    fx = self_x / hx  # -1 (edge) .. 0 (centre) .. 1 (edge)
    fz = self_z / hz
    return -fx * abs(fx) * 2.0, -fz * abs(fz) * 2.0


def _separation(targets):
    # Push away from nearby teammates so the swarm does not clump.
    sx, sz = 0.0, 0.0
    for t in targets:
        if t["type"] == "bot" and t["team"] == self_team and t["distance"] < 3.0:
            dx = self_x - t["x"]
            dz = self_z - t["z"]
            d = max(t["distance"], 0.05)
            sx += dx / (d * d)
            sz += dz / (d * d)
    return sx, sz


def _shared_focus(targets, dt):
    # Elect ONE enemy for the whole team via team_mem so fire concentrates.
    # The stored pick's priority distance decays so a dead/stale target is
    # replaced; the teammate nearest the current focus keeps it fresh.
    enemy = _nearest_enemy(targets)
    best_d = team_mem.get("focus_d", float("inf")) + 40.0 * dt
    if enemy is not None and enemy["distance"] < best_d:
        team_mem["focus_x"] = enemy["x"]
        team_mem["focus_z"] = enemy["z"]
        team_mem["focus_d"] = enemy["distance"]
    else:
        team_mem["focus_d"] = best_d
    return team_mem.get("focus_x"), team_mem.get("focus_z")


def think(dt):
    global _panic_angle, _panic_timer

    _panic_timer -= dt

    targets = scan(0)
    ax, az = _wall_avoid(targets)
    cx, cz = _steer_to_center()
    sx, sz = _separation(targets)
    fx, fz = _shared_focus(targets, dt)
    enemy = _nearest_enemy(targets)

    aggressive = (self_id % 2 == 0)  # even ids attack, odd ids defend/kite

    if self_hp < self_max_hp * LOW_HP_FRAC:
        # Fall back toward centre/rally with the team, still firing on focus.
        move(cx + ax + sx, cz + az + sz)
        if fx is not None:
            fire(fx - self_x, fz - self_z)
        return

    if enemy is not None:
        tx = fx if fx is not None else enemy["x"]
        tz = fz if fz is not None else enemy["z"]
        dx = tx - self_x
        dz = tz - self_z
        if aggressive or enemy["distance"] > 10.0:
            move(dx + ax + cx + sx, dz + az + cz + sz)   # close in / hold line
        else:
            move(-dx + ax + cx + sx, -dz + az + cz + sz)  # defenders kite out
        fire(dx, dz)
    else:
        if _panic_timer <= 0.0:
            _panic_angle = random.uniform(0, math.pi * 2)
            _panic_timer = 1.0 + random.random()
        move(math.cos(_panic_angle) + ax + cx + sx,
             math.sin(_panic_angle) + az + cz + sz)


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
# 6b) NAVIGATION: use arena_half_x / arena_half_z (arena is centred on 0,0) to
#     stop ramming the border. Add a centre-pull to your move() vector that
#     grows near the edge, and keep spacing from teammates.
# ----------------------------------------------------------------------------
# def _steer_to_center():
#     hx = max(arena_half_x, 1.0)
#     hz = max(arena_half_z, 1.0)
#     fx = self_x / hx            # -1 at one edge, 0 centre, +1 other edge
#     fz = self_z / hz
#     return -fx * abs(fx) * 2.0, -fz * abs(fz) * 2.0
#
# def _separation(targets):       # push away from nearby TEAMMATES
#     sx, sz = 0.0, 0.0
#     for t in targets:
#         if t["type"] == "bot" and t["team"] == self_team and t["distance"] < 3.0:
#             dx = self_x - t["x"]; dz = self_z - t["z"]
#             d = max(t["distance"], 0.05)
#             sx += dx / (d * d);  sz += dz / (d * d)
#     return sx, sz
# # then blend all steering vectors:  move(dx + ax + cx + sx, dz + az + cz + sz)
#
# 6c) TEAM COORDINATION via team_mem (a shared dict, SAME object for the team).
#     Always use team_mem.get(key, default); never assume a key exists.
#     FOCUS FIRE: elect one enemy the whole team shoots so targets die fast.
# ----------------------------------------------------------------------------
# def _shared_focus(targets, dt):
#     enemy = _nearest_enemy(targets)
#     best_d = team_mem.get("focus_d", float("inf")) + 40.0 * dt   # decay pick
#     if enemy is not None and enemy["distance"] < best_d:
#         team_mem["focus_x"] = enemy["x"]
#         team_mem["focus_z"] = enemy["z"]
#         team_mem["focus_d"] = enemy["distance"]
#     else:
#         team_mem["focus_d"] = best_d
#     return team_mem.get("focus_x"), team_mem.get("focus_z")
# # in think(): fx, fz = _shared_focus(targets, dt)
# #             if fx is not None: fire(fx - self_x, fz - self_z)
#
# 6d) ROLE SPLIT with self_id (unique per bot, stable for the match): send some
#     bots in to brawl and keep others back to defend/flank.
# ----------------------------------------------------------------------------
# aggressive = (self_id % 2 == 0)          # even ids attack, odd ids kite
# if aggressive or enemy["distance"] > 10.0:
#     move(dx + ax + cx + sx, dz + az + cz + sz)     # close in
# else:
#     move(-dx + ax + cx + sx, -dz + az + cz + sz)   # kite away
#
# 6) PITFALLS that the smoke test will reject (each one models have hit):
# ----------------------------------------------------------------------------
#    * This is PYTHON, not Lua. No 'local', no 'then', no 'end', no '--' comments.
#    * NEVER read self_x / self_z / self_team / self_hp / self_id /
#      arena_half_x / arena_half_z / team_mem at MODULE scope — they do not
#      exist until think() runs. Only read them inside think().
#      (init() actually must NOT read them either; only think() should.)
#    * team_mem is a shared dict: read with team_mem.get(key, default); never
#      assume a key exists, and never rebind the name (team_mem = ... is wrong).
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
