function init()
    return {
        left_weapon  = "AutoCannon",
        right_weapon = "Laser",
        armour       = 2
    }
end

function think(dt)
    local enemies = {}
    local allies = {}

    -- Categorize all bots within 20 units (expanded range)
    for _, obj in ipairs(scan(20)) do
        if obj.type == "bot" then
            if obj.team ~= self_team then
                table.insert(enemies, obj)
            else
                table.insert(allies, obj)
            end
        end
    end

    -- Prioritize closest enemy with health consideration
    local target = nil
    local min_dist = math.huge
    local min_health_ratio = 1.0
    for _, e in ipairs(enemies) do
        local health_ratio = e.hp / (e.max_hp or 1)
        if e.distance  min_dist or (e.distance == min_dist and health_ratio  min_health_ratio) then
            min_dist = e.distance
            min_health_ratio = health_ratio
            target = e
        end
    end

    -- If no enemies, prioritize allies with low health
    if not target and #allies  0 then
        local closest_ally = nil
        local min_ally_dist = math.huge
        local min_ally_health_ratio = 1.0
        for _, a in ipairs(allies) do
            local health_ratio = a.hp / (a.max_hp or 1)
            if a.distance  min_ally_dist or (a.distance == min_ally_dist and health_ratio  min_ally_health_ratio) then
                min_ally_dist = a.distance
                min_ally_health_ratio = health_ratio
                closest_ally = a
            end
        end
        target = closest_ally
    end

    -- Movement strategy with dynamic speed adjustment
    if target then
        local dx = target.x - self_x
        local dz = target.z - self_z
        local dist = math.sqrt(dx*dx + dz*dz)

        -- Dynamic movement based on distance and health
        if dist  10 then
            move(dx, dz)  -- Full speed for long range
        elseif dist  5 then
            move(dx * 0.7, dz * 0.7)  -- Moderate speed for mid range
        else
            -- Close combat: prioritize health
            if self_hp  self_max_hp * 0.7 then
                move(0, 0)  -- Stay and defend
            else
                move(dx * 0.5, dz * 0.5)  -- Slow approach
            end
        end

        -- Weapon selection with distance and health consideration
        if dist  12 then
            fire(dx, dz)  -- AutoCannon for long range
        elseif dist  6 then
            fire(dx, dz)  -- AutoCannon for mid range
        else
            -- Close range: use Laser if enemy is weak, AutoCannon if strong
            if target.hp / (target.max_hp or 1)  0.5 then
                fire(dx, dz)  -- Laser for finishing off weak enemies
            else
                fire(dx, dz)  -- AutoCannon for stronger enemies
            end
        end
    else
        -- Default movement if no target - random direction to avoid stagnation
        move(math.random(-1, 1), math.random(-1, 1))
    end

    -- Advanced wall avoidance with path planning
    local wall_avoidance = {dx = 0, dz = 0}
    for _, obj in ipairs(scan(4)) do
        if obj.type == "wall" and obj.distance  3 then
            local wall_dx = self_x - obj.x
            local wall_dz = self_z - obj.z
            local wall_dist = math.sqrt(wall_dx*wall_dx + wall_dz*wall_dz)
            if wall_dist  1.5 then
                -- Strong avoidance if very close to wall
                wall_avoidance.dx = wall_dx * 2
                wall_avoidance.dz = wall_dz * 2
            else
                -- Gentle avoidance if farther from wall
                wall_avoidance.dx = wall_dx * 0.5
                wall_avoidance.dz = wall_dz * 0.5
            end
        end
    end

    -- Combine movement with wall avoidance
    if target then
        local dx = target.x - self_x
        local dz = target.z - self_z
        local dist = math.sqrt(dx*dx + dz*dz)

        if dist  0.1 then  -- Only adjust if we're moving toward target
            move(dx + wall_avoidance.dx, dz + wall_avoidance.dz)
        else
            move(wall_avoidance.dx, wall_avoidance.dz)
        end
    else
        move(wall_avoidance.dx, wall_avoidance.dz)
    end
end