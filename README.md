# Llama Wars

A bot-battle arena built with C, Llama.cpp, Raylib, and Python.
Bots are scripted in Python and fight it out in a 3D arena. 
Only one class of robots are generated and improved by a local llama.cpp LLM server.

https://www.youtube.com/shorts/6yXtlWL2qBU


## Author

Leonardo Salvatore — [leonardosalvatore@gmail.com](mailto:leonardosalvatore@gmail.com)

## Requirements

- CMake ≥ 3.20
- Python 3 development headers (`python3-dev` / `python3-devel`)

## Build and run

```bash
cmake -B build
cmake --build build
```

## Run

```bash
./build/llama-wars
```

To use the LLM-driven bot, start a llama.cpp server first:

```bash
./start-llama-server.sh
```

## LLM bot context

Between generations the LLM rewrites `scripts/bot_llm.py`. Besides the combat
API (`move`, `fire`, `fire_weapon`, `scan`) and the `self_*` state, each bot's
`think(dt)` receives context for smarter behaviour:

- `self_id` — unique, stable id for the bot; used to split roles across the team.
- `arena_half_x` / `arena_half_z` — arena bounds (centred on `(0,0)`), so bots can
  steer toward the middle instead of ramming the border.
- `team_mem` — a shared dict, the same object for every bot on the team, used to
  coordinate (e.g. elect a focus-fire target, pick a rally point, assign roles).

These let generated bots navigate the ground and coordinate attack/defence with
teammates. The `EXAMPLES / COOKBOOK` block at the bottom of `scripts/bot_llm.py`
carries reference patterns (steer-to-center, teammate separation, shared focus
fire, role split) that the model reuses on each regeneration.

## Inspect overlay (press T)

Press **T** during a match to toggle an inspect overlay that renders what the
robots are "thinking" straight onto the battlefield:

- **Movement-intent arrow** — a cyan arrow from each bot showing the direction
  it is trying to drive (faint while holding position).
- **Turret aim ray + target ring** — an amber ray along the turret heading; when
  it lines up with a scanned enemy, that enemy gets a ground ring so you can see
  exactly who the bot is shooting at.
- **Team focus-fire marker** — a pulsing ring/crosshair at the shared
  `team_mem` focus target for the LLM team, with faint links from every living
  LLM bot to it, visualising how the swarm concentrates fire.
- **Scan lines** — the raw `scan()` hits (enemies and walls), dimmed so the
  cues above stay readable.

One line for reset, build and executing.

```bash
clear && git checkout scripts/bot_llm.py && cmake --build build && ./build/llama-wars
```

## License

Copyright (c) 2026 Leonardo Salvatore \<leonardosalvatore@gmail.com\>

This project is licensed under the **MIT License**.

See the [LICENSE](LICENSE) file for the full license text.
