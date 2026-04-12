-- bot_duelist.lua — Double AutoCannon, medium armour.
-- HP=200, speed=2.0. Advances slowly and hammers with two cannons.
-- Fires in movement direction while wandering; steers around walls.

function init()
    return {
        left_weapon  = "AutoCannon",
        right_weapon = "AutoCannon",
        armour       = 2
    }
end

math.randomseed()

local fire_cd      = math.random() * 0.25
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
    fire_cd      = fire_cd      - dt
    wander_timer = wander_timer - dt

    local targets = scan(16.0)
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
        if self_hp < self_max_hp * 0.15 then
            mx = -dx + ax;  mz = -dz + az
        else
            mx =  dx + ax;  mz =  dz + az
        end
        move(mx, mz)
        if enemy.distance < 8.0 and fire_cd <= 0.0 then
            fire(dx, dz)
            fire_cd = 0.25
        end
    else
        local al = math.sqrt(ax * ax + az * az)
        if al > 0.4 then
            wander_angle = math.atan(az, ax) + (math.random() - 0.5) * 0.5
            wander_timer = 0.5
        elseif wander_timer <= 0.0 then
            wander_angle = math.random() * math.pi * 2
            wander_timer = 1.2 + math.random()
        end
        local wx = math.cos(wander_angle) + ax
        local wz = math.sin(wander_angle) + az
        move(wx, wz)
    end
end
