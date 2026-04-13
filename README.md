# Llama Wars

A bot-battle arena built with C, Llama.cpp, Raylib, Lua.
Bots are scripted in Lua and fight it out in a 3D arena. 
Only one class of robots are generated and improved by a local llama.cpp LLM server.

https://www.youtube.com/watch?v=FMspkoXseRw

## Author

Leonardo Salvatore — [leonardosalvatore@gmail.com](mailto:leonardosalvatore@gmail.com)

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

One line for reset ,build and executing.

```bash
clear && git checkout scripts/bot_llm.lua && cmake --build build  && ./build/llama-wars
```

## License

Copyright (C) 2026 Leonardo Salvatore \<leonardosalvatore@gmail.com\>

This project is licensed under the **GNU General Public License v3.0 (GPL-3.0)**.

You are free to use, study, modify, and distribute this software, provided that:

- Any redistribution — modified or not — must be released under the same GPL-3.0 license.
- The original author (Leonardo Salvatore) must be credited.
- The complete source code must be made available alongside any distributed binary.

See the [LICENSE](LICENSE) file for the full license text, or visit <https://www.gnu.org/licenses/gpl-3.0.html>.