#!/usr/bin/env bash
# Build (if needed) and launch the Fox Tail emulator, then open the UI.
#
#   ./emulator/run.sh
#
# Native audio (CoreAudio) + a local web UI on http://localhost:4343.
# Uses CMake if available (nice for CLion); otherwise falls back to a direct
# clang++ compile so it works with zero extra installs.
set -euo pipefail

cd "$(dirname "$0")"

URL="http://localhost:4343"
BIN="build/foxtail-emu"

./render_panel.py

if command -v cmake >/dev/null 2>&1; then
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null
  cmake --build build --config Release
else
  echo "cmake not found — falling back to direct clang++ build"
  mkdir -p build
  clang++ -std=c++17 -O2 \
    -Ivendor -I.. \
    -DWEB_DIR="\"$PWD/web\"" \
    src/main.cpp \
    -framework CoreAudio -framework AudioToolbox -framework CoreFoundation \
    -lpthread \
    -o "$BIN"
fi

# Open the browser shortly after the server comes up.
( sleep 1; command -v open >/dev/null && open "$URL" || true ) &

echo "Fox Tail emulator -> $URL   (Ctrl-C to stop)"
exec "$BIN"
