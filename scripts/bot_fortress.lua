-- bot_fortress.lua — Double Laser, maximum armour.
-- HP=250, speed=0.8. Almost immobile; wins by continuous beam damage.
-- Sweeps lasers in movement direction while patrolling; steers around walls.

function init()
    return {
        left_weapon  = "Laser",
        right_weapon = "Laser",
        armour       = 3
    }
end

math.randomseed()

local fire_cd      = math.random() * 0.05
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

    local targets = scan(25.0)
    local ax, az  = wall_avoid(targets)

    local enemy, min_dist = nil, math.huge
    for _, t in ipairs(targets) do
        if t.type == "bot" and t.team ~= self_team and t.distance < min_dist then
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
        if fire_cd <= 0.0 then
            fire(dx, dz)
            fire_cd = 0.05
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
        if fire_cd <= 0.0 then
            fire(wx, wz)
            fire_cd = 0.1
        end
    end
end
