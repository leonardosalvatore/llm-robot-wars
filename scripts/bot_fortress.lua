-- bot_fortress.lua — Tracked laser fortress.
-- Build: tracks + tank body + 4 weapons (the maximum):
--   Laser (left), Laser (right), Laser (top), AutoCannon (top_rear).
-- The heaviest chassis. Almost immobile but pivots fast on its tracks.
-- Wins by continuous beam damage from three lasers; the rear AutoCannon
-- punishes anything that gets behind it.

function init()
    return {
        locomotion = "tracks",
        body       = "tank",
        weapons = {
            { type = "Laser",      mount = "left"     },
            { type = "Laser",      mount = "right"    },
            { type = "Laser",      mount = "top"      },
            { type = "AutoCannon", mount = "top_rear" },
        },
    }
end

math.randomseed()

local wander_angle = math.random() * math.pi * 2
local wander_timer = 0.0

local function wall_avoid(targets)
    local ax, az = 0, 0
    for _, r in ipairs(targets) do
        if r.type == "wall" and r.distance < 2.5 then
            local dx = self_x - r.x
            local dz = self_z - r.z
            local d  = math.max(r.distance, 0.05)
            ax = ax + dx / (d * d)
            az = az + dz / (d * d)
        end
    end
    return ax, az
end

function think(dt)
    wander_timer = wander_timer - dt

    local targets = scan(28.0)
    local ax, az  = wall_avoid(targets)

    local enemy, min_dist = nil, math.huge
    for _, t in ipairs(targets) do
        if t.type == "bot" and t.team == 6 and t.distance < min_dist then
            min_dist = t.distance
            enemy    = t
        end
    end

    if enemy then
        local dx = enemy.x - self_x
        local dz = enemy.z - self_z
        local mx = dx + ax
        local mz = dz + az
        move(mx, mz)
        -- All three lasers stay locked on; the AutoCannon (slot 4) only
        -- fires when the target is well in range, since its 0.6s reload
        -- otherwise wastes shots.
        fire_weapon(1, dx, dz)
        fire_weapon(2, dx, dz)
        fire_weapon(3, dx, dz)
        if enemy.distance < 14.0 then
            fire_weapon(4, dx, dz)
        end
    else
        local al = math.sqrt(ax * ax + az * az)
        if al > 0.4 then
            wander_angle = math.atan(az, ax) + (math.random() - 0.5) * 0.5
            wander_timer = 0.8
        elseif wander_timer <= 0.0 then
            wander_angle = math.random() * math.pi * 2
            wander_timer = 1.8 + math.random()
        end
        local wx = math.cos(wander_angle) + ax
        local wz = math.sin(wander_angle) + az
        move(wx, wz)
    end
end
