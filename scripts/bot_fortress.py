# bot_fortress.py — Tracked laser fortress.
# Build: tracks + tank body + 4 weapons (maximum):
#   Laser (left), Laser (right), Laser (top), AutoCannon (top_rear).
# Heaviest chassis. Almost immobile but pivots fast on its tracks.
# Continuous beam damage from three lasers; rear AutoCannon for close threats.
# fire_weapon uses 0-based index: 0=Laser/left, 1=Laser/right, 2=Laser/top, 3=AutoCannon/top_rear

import math
import random


def init():
    return {
        "locomotion": "tracks",
        "body": "tank",
        "weapons": [
            {"type": "Laser",      "mount": "left"},
            {"type": "Laser",      "mount": "right"},
            {"type": "Laser",      "mount": "top"},
            {"type": "AutoCannon", "mount": "top_rear"},
        ],
    }


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
    global _wander_angle, _wander_timer

    _wander_timer -= dt

    targets = scan(28.0)
    ax, az = _wall_avoid(targets)

    enemy, min_dist = None, float("inf")
    for t in targets:
        if t["type"] == "bot" and t["team"] == 6 and t["distance"] < min_dist:
            min_dist = t["distance"]
            enemy = t

    if enemy:
        dx = enemy["x"] - self_x
        dz = enemy["z"] - self_z
        move(dx + ax, dz + az)
        # Three lasers locked on at all times
        fire_weapon(0, dx, dz)
        fire_weapon(1, dx, dz)
        fire_weapon(2, dx, dz)
        # AutoCannon only fires within effective range (0.6s reload)
        if enemy["distance"] < 14.0:
            fire_weapon(3, dx, dz)
    else:
        al = math.sqrt(ax * ax + az * az)
        if al > 0.4:
            _wander_angle = math.atan2(az, ax) + (random.random() - 0.5) * 0.5
            _wander_timer = 0.8
        elif _wander_timer <= 0.0:
            _wander_angle = random.uniform(0, math.pi * 2)
            _wander_timer = 1.8 + random.random()
        wx = math.cos(_wander_angle) + ax
        wz = math.sin(_wander_angle) + az
        move(wx, wz)
