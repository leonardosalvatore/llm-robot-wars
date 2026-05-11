# bot_light.py — Fast scout walker.
# Build: 2legs + long_low body + 2x MachineGun (left/right).
# Lowest-weight chassis: highest base speed, low HP.
# Kites at range and sprays both MGs; backs off when the enemy gets close.

import math
import random


def init():
    return {
        "locomotion": "2legs",
        "body": "long_low",
        "weapons": [
            {"type": "MachineGun", "mount": "left"},
            {"type": "MachineGun", "mount": "right"},
        ],
    }


_fire_cd = random.uniform(0, 0.08)
_wander_angle = random.uniform(0, math.pi * 2)
_wander_timer = 0.0


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


def think(dt):
    global _fire_cd, _wander_angle, _wander_timer

    _fire_cd -= dt
    _wander_timer -= dt

    targets = scan(14.0)
    ax, az = _wall_avoid(targets)

    enemy, min_dist = None, float("inf")
    for t in targets:
        if t["type"] == "bot" and t["team"] != self_team and t["distance"] < min_dist:
            min_dist = t["distance"]
            enemy = t

    if enemy:
        dx = enemy["x"] - self_x
        dz = enemy["z"] - self_z
        if enemy["distance"] < 4.0:
            move(-dx + ax, -dz + az)
        else:
            move(dx + ax, dz + az)
        if _fire_cd <= 0.0:
            fire(dx, dz)
            _fire_cd = 0.08
    else:
        al = math.sqrt(ax * ax + az * az)
        if al > 0.4:
            _wander_angle = math.atan2(az, ax) + (random.random() - 0.5) * 0.5
            _wander_timer = 0.4
        elif _wander_timer <= 0.0:
            _wander_angle = random.uniform(0, math.pi * 2)
            _wander_timer = 0.6 + random.random()
        wx = math.cos(_wander_angle) + ax
        wz = math.sin(_wander_angle) + az
        move(wx, wz)
