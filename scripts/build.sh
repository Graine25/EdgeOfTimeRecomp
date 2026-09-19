#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

PRESET="${1:-linux-amd64-relwithdebinfo}"

if [ ! -f "../rexglue-sdk-dll/CMakeLists.txt" ]; then
    echo "[FAILED] ../rexglue-sdk-dll is not beside this repository (the preset's REXSDK_DIR)." >&2
    exit 1
fi

echo "[1/2] Configuring $PRESET..."
cmake --preset "$PRESET"

echo
echo "[2/2] Building (the SDK, codegen, then reeot)..."
cmake --build --preset "$PRESET" --parallel

echo
echo "Build complete: out/build/$PRESET/reeot"
