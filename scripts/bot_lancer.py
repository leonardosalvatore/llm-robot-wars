# bot_lancer.py — Wheeled artillery skirmisher.
# Build: wheels + tall body + 3 weapons:
#   AutoCannon (left), Laser (right), Laser (top_front).
# Uses fire_weapon() to switch weapons per range:
#   long range (>20 u): both lasers (index 1, 2) — fast, accurate
#   medium range:       all three weapons
#   short range (<6 u): cannon (index 0) + side laser (index 1)
#
# fire_weapon uses 0-based index: 0=AutoCannon, 1=Laser/right, 2=Laser/top_front

import math
import random


def init():
    return {
        "locomotion": "wheels",
        "body": "tall",
        "weapons": [
            {"type": "AutoCannon", "mount": "left"},
            {"type": "Laser",      "mount": "right"},
            {"type": "Laser",      "mount": "top_front"},
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

    targets = scan(25.0)
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
            move(-dx + ax, -dz + az)
        elif enemy["distance"] > 16.0:
            move(dx + ax, dz + az)
        else:
            move(ax, az)

        if enemy["distance"] > 20.0:
            fire_weapon(1, dx, dz)   # right laser
            fire_weapon(2, dx, dz)   # top_front laser
        elif enemy["distance"] < 6.0:
            fire_weapon(0, dx, dz)   # AutoCannon
            fire_weapon(1, dx, dz)   # right laser
        else:
            fire(dx, dz)             # everything
    else:
        al = math.sqrt(ax * ax + az * az)
        if al > 0.4:
            _wander_angle = math.atan2(az, ax) + (random.random() - 0.5) * 0.5
            _wander_timer = 0.6
        elif _wander_timer <= 0.0:
            _wander_angle = random.uniform(0, math.pi * 2)
            _wander_timer = 1.5 + random.random()
        wx = math.cos(_wander_angle) + ax
        wz = math.sin(_wander_angle) + az
        move(wx, wz)
