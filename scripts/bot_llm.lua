-- bot_llm.lua
-- Moves straight, scans, fires in a fixed direction.

function init()
    return {
        left_weapon  = "MachineGun",
        right_weapon = "MachineGun",
        armour       = 0
    }
end

local FIRE_DIR_X = 1.0
local FIRE_DIR_Z = 0.0

function think(dt)
    scan(8)
    move(1, 0)
    fire(FIRE_DIR_X, FIRE_DIR_Z)
end
