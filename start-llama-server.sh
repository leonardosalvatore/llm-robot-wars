#!/usr/bin/env bash
# Start llama-server with the bundled ROCm binary and GGUF model.
# Usage: ./start-llama-server.sh [model.gguf]

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LLAMA_DIR="${SCRIPT_DIR}/../llama.cpp/llama-b8532-bin-ubuntu-rocm-7.2-x64/llama-b8532"
#DEFAULT_MODEL="${LLAMA_DIR}/Ministral-3-8B-Reasoning-2512-Q5_K_M.gguf"
#DEFAULT_MODEL="${LLAMA_DIR}/Ministral-3-8B-Reasoning-2512-Q8_0.gguf"
#DEFAULT_MODEL="${LLAMA_DIR}/Qwen3.5-9b-Sushi-Coder-RL.Q4_K_M.gguf"
DEFAULT_MODEL="${LLAMA_DIR}/qwen2.5-coder-7b-instruct-q8_0.gguf"
#DEFAULT_MODEL="${LLAMA_DIR}/ruvltra-claude-code-0.5b-q4_k_m.gguf"

MODEL="${1:-$DEFAULT_MODEL}"

if [ -z "$MODEL" ]; then
    echo "ERROR: no model specified." >&2
    echo "  Pass one as: $0 /path/to/model.gguf" >&2
    echo "  Or uncomment a DEFAULT_MODEL line in this script." >&2
    exit 1
fi
if [ ! -f "$MODEL" ]; then
    echo "ERROR: model file not found: $MODEL" >&2
    exit 1
fi

cd "$LLAMA_DIR"

ln -s /opt/rocm/lib/libamdhip64.so.6 ./libamdhip64.so.7 2>/dev/null
ln -s /opt/rocm/lib/libhipblas.so.2 ./libhipblas.so.3 2>/dev/null
ln -s /opt/rocm/lib/librocblas.so.4 ./librocblas.so.5 2>/dev/null

export LD_LIBRARY_PATH=.:/opt/rocm/lib:/opt/rocm/lib64:$LD_LIBRARY_PATH

echo "Starting llama-server..."
echo "  Binary : ${LLAMA_DIR}/llama-server"
echo "  Model  : ${MODEL}"
echo "  Port   : 8080"
echo ""

HIP_VISIBLE_DEVICES=0 HSA_OVERRIDE_GFX_VERSION=10.3.0 \
    ./llama-server \
    --model "$MODEL" \
    --host 0.0.0.0 \
    --port 8080 \
    -ngl 99 \
    -c 8192
