function init()
    return {
        left_weapon  = "AutoCannon",
        right_weapon = "Laser",
        armour       = 2
    }
end

function think(dt)
    local scan_result = scan(15)
    local enemies = {}
    local walls = {}

    for _, entry in ipairs(scan_result) do
        if entry.type == "bot" then
            table.insert(enemies, entry)
        elseif entry.type == "wall" then
            table.insert(walls, entry)
        end
    end

    -- Sort enemies by distance (closest first), then by HP (lowest first)
    table.sort(enemies, function(a, b)
        if a.distance == b.distance then
            return a.hp < b.hp
        end
        return a.distance < b.distance
    end)

    local move_dx = 0
    local move_dz = 0
    local fire_dx = 0
    local fire_dz = 0

    local should_fire = false

    if #enemies > 0 then
        local target = enemies[1]
        local dx = target.x - self_x
        local dz = target.z - self_z
        local distance = math.sqrt(dx * dx + dz * dz)

        -- Calculate firing direction (normalized)
        fire_dx, fire_dz = dx / distance, dz / distance

        -- Move toward target if out of range, otherwise maintain position
        if distance < 5 then
            move_dx = -dx / distance * 0.1
            move_dz = -dz / distance * 0.1
        elseif distance > 15 then
            move_dx = dx / distance * 0.1
            move_dz = dz / distance * 0.1
        end

        -- Wall collision avoidance
        if #walls > 0 then
            local closest_wall = nil
            local closest_wall_distance = 9999

            for _, wall in ipairs(walls) do
                if wall.distance < closest_wall_distance then
                    closest_wall_distance = wall.distance
                    closest_wall = wall
                end
            end

            local wall_dx = closest_wall.x - self_x
            local wall_dz = closest_wall.z - self_z
            local dot_product = dx * wall_dx + dz * wall_dz

            if dot_product > 0 then
                move_dx = move_dx - wall_dx * 0.05
                move_dz = move_dz - wall_dz * 0.05
            end
        end

        move(move_dx, move_dz)

        -- Fire weapons
        fire(fire_dx, fire_dz, 0)
        should_fire = true
    else
        -- Random movement when no enemies
        if math.random() < 0.05 then
            move_dx = math.random() - 0.5
            move_dz = math.random() - 0.5
        end

        move(move_dx, move_dz)
    end

    -- Fire weapons (already done above if enemies exist)
    if should_fire then
        fire(fire_dx, fire_dz, 0)
    end
end