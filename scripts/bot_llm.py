# big_robot_autocannon.py — "titan-auto": big tank with autocannons
import math

def init():
    return {
        "locomotion": "wheels",
        "body": "tank",
        "weapons": [
            {"type": "AutoCannon", "mount": "left"},
            {"type": "AutoCannon", "mount": "right"},
        ],
    }

# Turret turn rate for autocannon (4 rad/s per spec)
_TURRET_TURN = 4.0

def think(dt):
    targets = scan(0)

    best_target = None
    best_dist = float("inf")

    for t in targets:
        if t["type"] == "bot" and t["team"] != self_team:
            d = t["distance"]
            if d < best_dist:
                best_dist = d
                best_target = t

    if best_target:
        dx = best_target["x"] - self_x
        dz = best_target["z"] - self_z
        # fire both autocannons every frame; engine limits by cooldown
        fire(dx, dz)