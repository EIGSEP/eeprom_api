#!/bin/bash

# Build script for the AT28BV64B EEPROM firmware (Raspberry Pi Pico 2 / RP2350)
#
# Produces the UF2 images used to program the EEPROM:
#   build/program_paths/program_paths.uf2
#   build/program_paths/select_path.uf2
#   build/program_paths/test_paths.uf2
#   build/test/test_eeprom.uf2
#
# Usage:
#   ./build.sh            # build everything (all UF2s + libat28bv64b.a)
#   ./build.sh <target>   # build one target, e.g. ./build.sh program_paths
#
# Unlike the pico-firmware repo, this project does NOT vendor pico-sdk. If
# PICO_SDK_PATH is unset, this script falls back to the SDK vendored in the
# sibling pico-firmware checkout.

set -euo pipefail

echo "==================================="
echo "Building AT28BV64B EEPROM Firmware"
echo "==================================="
echo "Target: Raspberry Pi Pico 2 (RP2350)"
echo "==================================="

# Always operate relative to this script, not the caller's cwd.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

BUILD_DIR="$SCRIPT_DIR/build"

# --- Resolve PICO_SDK_PATH -------------------------------------------------
# This project's CMakeLists.txt hard-requires $ENV{PICO_SDK_PATH} at configure
# time (there is no auto-detect in the CMake itself), so establish it here.
sdk_ok() { [ -f "$1/external/pico_sdk_import.cmake" ]; }

if [ -n "${PICO_SDK_PATH:-}" ] && sdk_ok "$PICO_SDK_PATH"; then
    PICO_SDK_PATH="$(realpath "$PICO_SDK_PATH")"
    echo "Using PICO_SDK_PATH from environment: $PICO_SDK_PATH"
elif sdk_ok "$SCRIPT_DIR/pico-sdk"; then
    PICO_SDK_PATH="$(realpath "$SCRIPT_DIR/pico-sdk")"
    echo "Using local Pico SDK: $PICO_SDK_PATH"
elif sdk_ok "$SCRIPT_DIR/../pico-firmware/pico-sdk"; then
    PICO_SDK_PATH="$(realpath "$SCRIPT_DIR/../pico-firmware/pico-sdk")"
    echo "Using sibling pico-firmware Pico SDK: $PICO_SDK_PATH"
else
    echo "ERROR: PICO_SDK_PATH is not set and no local pico-sdk was found."
    echo "Set PICO_SDK_PATH to a pico-sdk >= 2.0.0 checkout, or place one at"
    echo "  $SCRIPT_DIR/pico-sdk, or clone pico-firmware as a sibling directory."
    exit 1
fi
export PICO_SDK_PATH

# --- Drop a stale / foreign CMake cache ------------------------------------
# The build/ dir is checked into this repo and may have been configured on a
# different machine (e.g. a Mac). A cache that points at a different SDK path
# or a compiler that no longer exists cannot be reused — wipe and reconfigure.
CACHE="$BUILD_DIR/CMakeCache.txt"
if [ -f "$CACHE" ]; then
    cached_sdk="$(sed -n 's/^PICO_SDK_PATH:PATH=//p' "$CACHE" | head -1 || true)"
    cached_cxx="$(sed -n 's/^CMAKE_CXX_COMPILER:[^=]*=//p' "$CACHE" | head -1 || true)"
    if [ "$cached_sdk" != "$PICO_SDK_PATH" ] || { [ -n "$cached_cxx" ] && [ ! -x "$cached_cxx" ]; }; then
        echo "Detected a stale build/ cache (configured elsewhere) — removing it for a clean configure."
        echo "  cached SDK:      ${cached_sdk:-<none>}"
        echo "  cached compiler: ${cached_cxx:-<none>}"
        rm -rf "$BUILD_DIR"
    fi
fi

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# --- Configure -------------------------------------------------------------
echo "Configuring with CMake for Pico 2..."
cmake .. -DPICO_BOARD=pico2

# --- Build -----------------------------------------------------------------
TARGET="${1:-}"
if [ -n "$TARGET" ]; then
    echo "Building target: $TARGET"
    make -j"$(nproc)" "$TARGET"
else
    echo "Building all targets..."
    make -j"$(nproc)"
fi

# --- Report outputs --------------------------------------------------------
echo "==================================="
mapfile -t UF2S < <(find "$BUILD_DIR" -name '*.uf2' | sort)
if [ "${#UF2S[@]}" -gt 0 ]; then
    echo "✅ Build successful! Produced UF2 image(s):"
    for f in "${UF2S[@]}"; do
        printf '  %s\n' "$f"
    done
    echo ""
    echo "Flash a single Pico 2 by holding BOOTSEL, plugging in USB, and copying"
    echo "the .uf2 onto the RPI-RP2 drive (no picotool required)."
else
    echo "❌ Build finished but no .uf2 was produced."
    exit 1
fi
echo "==================================="
