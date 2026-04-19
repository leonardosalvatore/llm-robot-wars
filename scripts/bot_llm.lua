-- bot_llm.lua — "wall-bumper": wander in a fixed direction; whenever the
-- bot stalls against a wall (or sees one very close) it picks a brand new
-- random direction. Dual AutoCannon; fires only at the nearest visible enemy.

function init()
    return {
        left_weapon  = "AutoCannon",
        right_weapon = "AutoCannon",
        armour       = 2,
    }
end

math.randomseed(os.time()
                + math.floor((self_x or 0) * 1000)
                + math.floor((self_z or 0) * 1000))

local heading     = math.random() * math.pi * 2
local prev_x, prev_z
local stall_time  = 0.0
local SCAN_RADIUS = 12.0
local STALL_SECS  = 0.25

local function pick_new_heading(avoid_dx, avoid_dz)
    local base = math.random() * math.pi * 2
    if avoid_dx and avoid_dz then
        local away = math.atan(avoid_dz, avoid_dx)
        if math.random() < 0.6 then
            base = away + (math.random() - 0.5) * (math.pi * 0.5)
        end
    end
    heading = base
end

function think(dt)
    if prev_x ~= nil then
        local dx = self_x - prev_x
        local dz = self_z - prev_z
        if (dx * dx + dz * dz) < (0.02 * 0.02) then
            stall_time = stall_time + dt
        else
            stall_time = 0.0
        end
    end
    prev_x, prev_z = self_x, self_z

    local targets = scan(SCAN_RADIUS)

    local wall_dx, wall_dz, wall_dist = nil, nil, math.huge
    local enemy, enemy_dist = nil, math.huge
    for i = 1, #targets do
        local t = targets[i]
        if t.type == "wall" then
            if t.distance < wall_dist then
                wall_dist = t.distance
                wall_dx   = self_x - t.x
                wall_dz   = self_z - t.z
            end
        elseif t.type == "bot" and t.team ~= self_team then
            if t.distance < enemy_dist then
                enemy_dist = t.distance
                enemy      = t
            end
        end
    end

    if stall_time >= STALL_SECS or (wall_dist < 1.5) then
        pick_new_heading(wall_dx, wall_dz)
        stall_time = 0.0
    end

    move(math.cos(heading), math.sin(heading))

    -- Fire ONLY when an enemy is visible. Engine caps AutoCannon at 0.6s, so
    -- calling fire() every tick is safe: excess shots are silently dropped.
    if enemy then
        fire(enemy.x - self_x, enemy.z - self_z)
    end
end
