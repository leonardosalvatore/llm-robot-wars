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

One line for reset, build and executing.

```bash
clear && git checkout scripts/bot_llm.py && cmake --build build && ./build/llama-wars
```

## License

Copyright (c) 2026 Leonardo Salvatore \<leonardosalvatore@gmail.com\>

This project is licensed under the **MIT License**.

See the [LICENSE](LICENSE) file for the full license text.
