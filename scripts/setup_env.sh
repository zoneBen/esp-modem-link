#!/usr/bin/env bash
# Setup build environment for esp-modem-link (Git Bash / MSYS2)
# Usage: source scripts/setup_env.sh

# CMake (from ESP-IDF toolchain)
CMAKE_PATH="/d/ESP32/Espressif/tools/cmake/3.30.2/bin"
export PATH="$CMAKE_PATH:$PATH"

# Ninja (from ESP-IDF toolchain)
NINJA_DIR="/d/ESP32/Espressif/tools/ninja"
if [ -d "$NINJA_DIR" ]; then
  NINJA_BIN=$(find "$NINJA_DIR" -name "ninja.exe" -type f 2>/dev/null | head -1)
  if [ -n "$NINJA_BIN" ]; then
    export PATH="$(dirname "$NINJA_BIN"):$PATH"
  fi
fi

# MSVC (Visual Studio 2022 Community)
MSVC_BIN="/c/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.43.34808/bin/Hostx64/x64"
if [ -d "$MSVC_BIN" ]; then
  export PATH="$MSVC_BIN:$PATH"
fi

echo "=== Environment ready ==="
which cmake 2>/dev/null && echo "  cmake: $(cmake --version 2>/dev/null | head -1)"
which cl 2>/dev/null && echo "  cl:    $(cl 2>&1 | head -1)"
echo ""
