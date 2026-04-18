-- bot_llm.lua — minimal baseline.
-- Wanders in a slowly changing direction and fires in random directions.
-- Intentionally simple: barely uses scan() so the LLM has maximum room to
-- propose improvements. Different bots behave differently thanks to the seed
-- being mixed with their spawn position.

function init()
    return {
        left_weapon  = "MachineGun",
        right_weapon = "Laser",
        armour       = 1,
    }
end

math.randomseed(os.time()
                + math.floor((self_x or 0) * 1000)
                + math.floor((self_z or 0) * 1000))

local wander_angle = math.random() * math.pi * 2
local wander_timer = 0.0
local fire_timer   = 0.0

function think(dt)
    wander_timer = wander_timer - dt
    fire_timer   = fire_timer   - dt

    if wander_timer <= 0.0 then
        wander_angle = wander_angle + (math.random() - 0.5) * 1.2
        wander_timer = 0.5 + math.random() * 1.0
    end

    move(math.cos(wander_angle), math.sin(wander_angle))

    if fire_timer <= 0.0 then
        local fa = math.random() * math.pi * 2
        fire(math.cos(fa), math.sin(fa))
        -- Engine caps per-weapon fire rate; this timer just saves a few calls.
        fire_timer = 0.2
    end
end
