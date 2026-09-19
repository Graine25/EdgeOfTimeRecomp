#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

if [ "$(uname -s)" = "Darwin" ]; then
    PRESET="${1:-mac-arm64-relwithdebinfo}"
else
    PRESET="${1:-linux-amd64-relwithdebinfo}"
fi

# REXSDK_DIR (an SDK source tree) or CMAKE_PREFIX_PATH (a nightly SDK package).
if [ -n "${REXSDK_DIR:-}" ]; then
    if [ ! -f "$REXSDK_DIR/CMakeLists.txt" ]; then
        echo "[FAILED] REXSDK_DIR=$REXSDK_DIR is not a rexglue-sdk checkout." >&2
        exit 1
    fi
elif [ -z "${CMAKE_PREFIX_PATH:-}" ]; then
    echo "[FAILED] Set REXSDK_DIR to a rexglue-sdk checkout, or CMAKE_PREFIX_PATH to an unpacked" >&2
    echo "         nightly SDK package (github.com/rexglue/rexglue-sdk/releases)." >&2
    exit 1
fi

echo "[1/2] Configuring $PRESET..."
cmake --preset "$PRESET"

echo
echo "[2/2] Building (the SDK, codegen, then reeot)..."
cmake --build --preset "$PRESET" --parallel

echo
echo "Build complete: out/build/$PRESET/EdgeOfTimeRecomp"
