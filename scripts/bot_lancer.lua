-- bot_lancer.lua — Wheeled artillery skirmisher.
-- Build: wheels + tall body + 3 weapons:
--   AutoCannon (left), Laser (right), Laser (top_front).
-- Uses fire_weapon() to switch weapons per range:
--   long range (>20 u): both lasers (top_front + right) only — fast, accurate;
--   medium range:       all three weapons;
--   short range (<6 u): drops the top laser, uses cannon + side laser.

function init()
    return {
        locomotion = "wheels",
        body       = "tall",
        weapons = {
            { type = "AutoCannon", mount = "left"      },
            { type = "Laser",      mount = "right"     },
            { type = "Laser",      mount = "top_front" },
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

    local targets = scan(25.0)
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
        local mx, mz
        if enemy.distance < 5.0 then
            mx = -dx + ax;  mz = -dz + az  -- back away from melee
        elseif enemy.distance > 16.0 then
            mx =  dx + ax;  mz =  dz + az  -- close gap toward sniping range
        else
            mx =  ax;       mz =  az
        end
        move(mx, mz)

        if enemy.distance > 20.0 then
            fire_weapon(2, dx, dz)  -- right laser
            fire_weapon(3, dx, dz)  -- top_front laser
        elseif enemy.distance < 6.0 then
            fire_weapon(1, dx, dz)  -- AutoCannon
            fire_weapon(2, dx, dz)  -- right laser
        else
            fire(dx, dz)            -- everything
        end
    else
        local al = math.sqrt(ax * ax + az * az)
        if al > 0.4 then
            wander_angle = math.atan(az, ax) + (math.random() - 0.5) * 0.5
            wander_timer = 0.6
        elseif wander_timer <= 0.0 then
            wander_angle = math.random() * math.pi * 2
            wander_timer = 1.5 + math.random()
        end
        local wx = math.cos(wander_angle) + ax
        local wz = math.sin(wander_angle) + az
        move(wx, wz)
    end
end
