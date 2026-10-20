#!/bin/sh
# Cross-compile a self-contained Windows build (townplay.exe, SDL2 linked in) from Linux.
# Uses zig as the cross compiler and the official SDL2 MinGW development package.
# Both are downloaded into .deps/ on first use; set ZIG or SDL2_MINGW to use existing ones.
set -e
cd "$(dirname "$0")"

ZIG_VERSION=0.13.0
SDL_VERSION=2.30.9
DEPS=.deps

if [ -z "$ZIG" ]; then
    ZIG="$DEPS/zig-linux-x86_64-$ZIG_VERSION/zig"
    if [ ! -x "$ZIG" ]; then
        mkdir -p "$DEPS"
        curl -fL -o "$DEPS/zig.tar.xz" "https://ziglang.org/download/$ZIG_VERSION/zig-linux-x86_64-$ZIG_VERSION.tar.xz"
        tar -xJf "$DEPS/zig.tar.xz" -C "$DEPS"
    fi
fi
if [ -z "$SDL2_MINGW" ]; then
    SDL2_MINGW="$DEPS/SDL2-$SDL_VERSION/x86_64-w64-mingw32"
    if [ ! -d "$SDL2_MINGW" ]; then
        mkdir -p "$DEPS"
        curl -fL -o "$DEPS/sdl2.tar.gz" \
            "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VERSION/SDL2-devel-$SDL_VERSION-mingw.tar.gz"
        tar -xzf "$DEPS/sdl2.tar.gz" -C "$DEPS"
    fi
fi

mkdir -p dist/windows
"$ZIG" cc -target x86_64-windows-gnu -O2 -std=c99 -Wall \
    -I"$SDL2_MINGW/include/SDL2" -Dmain=SDL_main \
    src/*.c \
    "$SDL2_MINGW/lib/libSDL2main.a" "$SDL2_MINGW/lib/libSDL2.a" \
    -lgdi32 -lwinmm -lsetupapi -limm32 -lole32 -loleaut32 -lversion -luuid \
    -lshell32 -luser32 -lkernel32 -ladvapi32 \
    -Wl,--subsystem,windows \
    -o dist/windows/townplay.exe
rm -f dist/windows/townplay.pdb
echo "built dist/windows/townplay.exe"
