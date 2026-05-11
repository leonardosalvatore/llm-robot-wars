import math
import random


def init():
    return {
        "locomotion": "wheels",
        "body": "cube",
        "weapons": [
            {"type": "MachineGun", "mount": "left"},
            {"type": "AutoCannon", "mount": "right"},
        ],
    }


_move_angle = random.uniform(0, math.pi * 2)
_move_timer = 0.0


def think(dt):
    global _move_angle, _move_timer

    _move_timer -= dt
    if _move_timer <= 0.0:
        _move_angle = random.uniform(0, math.pi * 2)
        _move_timer = 1.0

    move(math.cos(_move_angle), math.sin(_move_angle))

    targets = scan(0)
    for t in targets:
        if t["type"] == "bot" and t["team"] != self_team:
            fire(t["x"] - self_x, t["z"] - self_z)
            break
