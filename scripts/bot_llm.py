import math
import random

def init():
    return {
        "locomotion": "wheels",
        "body": "flat",
        "weapons": [
            {"type": "AutoCannon", "mount": "left"},
            {"type": "AutoCannon", "mount": "right"},
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
    if not targets:
        _panic_timer = 1.0
        move(_panic_angle, 0)
        return

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