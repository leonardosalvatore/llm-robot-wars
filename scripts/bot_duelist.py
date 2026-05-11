# bot_duelist.py — Triple-cannon walking turret.
# Build: 4legs + tower body + 3 AutoCannons (left, right, top).
# Tall, narrow profile. Holds ground at medium range and
# hammers the enemy with three synchronised cannons.

import math
import random


def init():
    return {
        "locomotion": "4legs",
        "body": "tower",
        "weapons": [
            {"type": "AutoCannon", "mount": "left"},
            {"type": "AutoCannon", "mount": "right"},
            {"type": "AutoCannon", "mount": "top"},
        ],
    }


_fire_cd = random.uniform(0, 0.25)
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

    targets = scan(16.0)
    ax, az = _wall_avoid(targets)

    enemy, min_dist = None, float("inf")
    for t in targets:
        if t["type"] == "bot" and t["team"] != self_team and t["distance"] < min_dist:
            min_dist = t["distance"]
            enemy = t

    if enemy:
        dx = enemy["x"] - self_x
        dz = enemy["z"] - self_z
        if enemy["distance"] < 5.0:
            # Strafe: keep enemy near medium range
            move(-dz + ax, dx + az)
        elif enemy["distance"] > 10.0:
            move(dx + ax, dz + az)
        else:
            move(ax, az)
        if enemy["distance"] < 12.0 and _fire_cd <= 0.0:
            fire(dx, dz)
            _fire_cd = 0.20
    else:
        al = math.sqrt(ax * ax + az * az)
        if al > 0.4:
            _wander_angle = math.atan2(az, ax) + (random.random() - 0.5) * 0.5
            _wander_timer = 0.5
        elif _wander_timer <= 0.0:
            _wander_angle = random.uniform(0, math.pi * 2)
            _wander_timer = 1.2 + random.random()
        wx = math.cos(_wander_angle) + ax
        wz = math.sin(_wander_angle) + az
        move(wx, wz)
