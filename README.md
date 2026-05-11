# Llama Wars

A bot-battle arena built with C, Llama.cpp, Raylib, and Python.
Bots are scripted in Python and fight it out in a 3D arena. 
Only one class of robots are generated and improved by a local llama.cpp LLM server.

https://youtube.com/shorts/_TR7kF-zSt4?feature=share


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

One line for reset, build and executing.

```bash
clear && git checkout scripts/bot_llm.py && cmake --build build && ./build/llama-wars
```

## License

Copyright (c) 2026 Leonardo Salvatore \<leonardosalvatore@gmail.com\>

This project is licensed under the **MIT License**.

See the [LICENSE](LICENSE) file for the full license text.
