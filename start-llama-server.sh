#!/usr/bin/env bash
# Start llama-server with the bundled ROCm binary and GGUF model.
# Usage: ./start-llama-server.sh [model.gguf]

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
#LLAMA_DIR="${SCRIPT_DIR}/../llama.cpp/llama-b8532-bin-ubuntu-rocm-7.2-x64/llama-b8532"
LLAMA_DIR="${SCRIPT_DIR}/../llama.cpp/llama-b9940-bin-ubuntu-rocm-7.2-x64/llama-b9940"

# Non-reasoning coder model: no hidden chain-of-thought, follows the strict
# API/output-format rules, best quality/latency of the available set.
DEFAULT_MODEL="${LLAMA_DIR}/qwen2.5-coder-7b-instruct-q8_0.gguf"
#DEFAULT_MODEL="${LLAMA_DIR}/Qwen3.5-9b-Sushi-Coder-RL.Q4_K_M.gguf"   # smaller/faster fallback
#DEFAULT_MODEL="${LLAMA_DIR}/Ministral-3-8B-Reasoning-2512-Q5_K_M.gguf" # reasoning model: slow (long CoT)
#DEFAULT_MODEL="${LLAMA_DIR}/Ministral-3-8B-Reasoning-2512-Q8_0.gguf"
#DEFAULT_MODEL="${LLAMA_DIR}/ruvltra-claude-code-0.5b-q4_k_m.gguf"

MODEL="${1:-$DEFAULT_MODEL}"

# Speculative decoding.
#
# Draft-model speculation (a tiny model drafts, the 7B verifies) is DISABLED
# here because this particular qwen2.5-coder-7b GGUF ships a nonstandard vocab
# (note the '</s>' @ 128247 warning at load) that does not match the stock
# Qwen2.5-0.5B draft -> "target and draft vocabs are not compatible".
# To use a draft model you'd need a matched pair (e.g. b9940's built-in
# preset --fim-qwen-7b-spec, which downloads a matching 7B + 0.5B).
#DRAFT_MODEL="${LLAMA_DIR}/ruvltra-claude-code-0.5b-q4_k_m.gguf"
DRAFT_MODEL=""

# Instead we use N-GRAM (prompt-lookup) speculation: no draft model, no vocab
# constraint. It drafts tokens by matching repeated n-grams already in the
# context. Ideal here because each regeneration copies large verbatim spans
# (the current script + the cookbook block) straight from the prompt.
NGRAM_SPEC=1

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

# Speculative-decoding flags. --spec-type defaults to 'none' in b9940, so it
# must be opted into explicitly.
SPEC_ARGS=()
if [ -n "$DRAFT_MODEL" ] && [ -f "$DRAFT_MODEL" ]; then
    # Draft-model speculation (needs a vocab-matched draft; see note above).
    SPEC_ARGS=(--spec-type draft-simple --model-draft "$DRAFT_MODEL" -ngld 99 \
               --spec-draft-n-max 16 --spec-draft-n-min 1)
elif [ "${NGRAM_SPEC:-0}" = "1" ]; then
    # N-gram / prompt-lookup speculation: no draft model, no vocab constraint.
    SPEC_ARGS=(--spec-type ngram-simple)
fi

echo "Starting llama-server..."
echo "  Binary : ${LLAMA_DIR}/llama-server"
echo "  Model  : ${MODEL}"
if [ -n "$DRAFT_MODEL" ] && [ -f "$DRAFT_MODEL" ]; then
    echo "  Draft  : ${DRAFT_MODEL}"
elif [ ${#SPEC_ARGS[@]} -gt 0 ]; then
    echo "  Spec   : ngram-simple (prompt-lookup, no draft model)"
fi
echo "  Port   : 8080"
echo ""

HIP_VISIBLE_DEVICES=0 HSA_OVERRIDE_GFX_VERSION=10.3.0 \
    ./llama-server \
    --model "$MODEL" \
    --host 0.0.0.0 \
    --port 8080 \
    -ngl 99 \
    -c 8192 \
    -fa on \
    --cache-reuse 256 \
    "${SPEC_ARGS[@]}"
