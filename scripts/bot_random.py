# bot_random.py — Single-gun walker, drunken AI.
# Build: 2legs + cube body + 1x MachineGun mounted on top.
# Wanders in random directions and fires at random angles.
# Steers off walls only just enough to not get pinned.

import math
import random


def init():
    return {
        "locomotion": "2legs",
        "body": "cube",
        "weapons": [
            {"type": "MachineGun", "mount": "top"},
        ],
    }


_move_angle = random.uniform(0, math.pi * 2)
_move_timer = 0.0
_fire_timer = random.uniform(0, 0.5)


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
    global _move_angle, _move_timer, _fire_timer

    _move_timer -= dt
    if _move_timer <= 0.0:
        _move_angle = random.uniform(0, math.pi * 2)
        _move_timer = 0.8 + random.random() * 1.5

    targets = scan(2.5)
    ax, az = _wall_avoid(targets)

    al = math.sqrt(ax * ax + az * az)
    if al > 0.4:
        _move_angle = math.atan2(az, ax) + (random.random() - 0.5) * 0.6
        _move_timer = 0.5

    mx = math.cos(_move_angle) + ax
    mz = math.sin(_move_angle) + az
    move(mx, mz)

    _fire_timer -= dt
    if _fire_timer <= 0.0:
        a = random.uniform(0, math.pi * 2)
        fire(math.cos(a), math.sin(a))
        _fire_timer = 0.12
