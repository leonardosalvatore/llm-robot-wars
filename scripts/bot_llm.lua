function init()
    return {
        left_weapon  = "MachineGun",
        right_weapon = "MachineGun",
        armour       = 0
    }
end

function think(dt)
    move(1, 0)
    fire(1, 0)
end
