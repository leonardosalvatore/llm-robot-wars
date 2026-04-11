-- bot_random.lua
-- Wanders in random directions and fires both weapons indiscriminately.
-- Now steers away from walls detected by scan().

function init()
    return {
        left_weapon  = "MachineGun",
        right_weapon = "MachineGun",
        armour       = 0
    }
end

math.randomseed()

local move_angle = math.random() * math.pi * 2
local move_timer = 0.0
local fire_timer = math.random() * 0.5

-- Returns (ax, az) repulsion vector from nearby walls.
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
    move_timer = move_timer - dt
    if move_timer <= 0.0 then
        move_angle = math.random() * math.pi * 2
        move_timer = 0.8 + math.random() * 1.5
    end

    -- Short scan just for wall avoidance
    local targets = scan(2.5)
    local ax, az  = wall_avoid(targets)

    -- If strong wall presence, redirect wander angle away
    local al = math.sqrt(ax * ax + az * az)
    if al > 0.4 then
        move_angle = math.atan(az, ax) + (math.random() - 0.5) * 0.6
        move_timer = 0.5
    end

    local mx = math.cos(move_angle) + ax
    local mz = math.sin(move_angle) + az
    move(mx, mz)

    fire_timer = fire_timer - dt
    if fire_timer <= 0.0 then
        local a = math.random() * math.pi * 2
        fire(math.cos(a), math.sin(a))
        fire_timer = 0.08
    end
end
