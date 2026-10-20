#!/bin/sh
# Build the Linux player (needs a C compiler, CMake and the SDL2 development package).
set -e
cd "$(dirname "$0")"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
mkdir -p dist/linux
cp build/townplay dist/linux/townplay
echo "built dist/linux/townplay"
