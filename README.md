# Llama Wars

A bot-battle arena built with C, Llama.cpp, Raylib, Lua.
Bots are scripted in Lua and fight it out in a 3D arena. 
Only one class of robots are generated and improved by a local llama.cpp LLM server.

https://www.youtube.com/watch?v=FMspkoXseRw

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