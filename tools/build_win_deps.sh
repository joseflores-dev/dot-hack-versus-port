#!/bin/bash
# Compila SDL2 y glslang para Windows x64 con llvm-mingw (portable) -> sysroot-win/
# Correr desde Git Bash:  tools/build_win_deps.sh
set -e
unset MSYS_NO_PATHCONV
cd "$(dirname "$0")/.."
P=$PWD
TC=$P/toolchain
MG=$(cd $TC/llvm-mingw-* && pwd)
export PATH="$MG/bin:$PATH"
CM=$TC/cmake-4.4.5-windows-x86_64/bin/cmake
COMMON=(-G Ninja -DCMAKE_MAKE_PROGRAM=$TC/ninja.exe -DCMAKE_BUILD_TYPE=Release
        -DCMAKE_C_COMPILER=$MG/bin/x86_64-w64-mingw32-clang.exe -DCMAKE_CXX_COMPILER=$MG/bin/x86_64-w64-mingw32-clang++.exe
        -DCMAKE_RC_COMPILER=$MG/bin/x86_64-w64-mingw32-windres.exe -DCMAKE_INSTALL_PREFIX=$P/sysroot-win)

$CM -S deps/SDL-release-2.32.10 -B build-sdl2-win "${COMMON[@]}" -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF > /dev/null
$CM --build build-sdl2-win > build-sdl2-win.log 2>&1 || { grep -E "error" build-sdl2-win.log | head; exit 1; }
$CM --install build-sdl2-win > /dev/null

$CM -S deps/glslang-16.6.0 -B build-glslang-win "${COMMON[@]}" -DENABLE_OPT=OFF -DGLSLANG_TESTS=OFF \
    -DENABLE_GLSLANG_BINARIES=OFF -DBUILD_SHARED_LIBS=OFF -DGLSLANG_ENABLE_INSTALL=ON > /dev/null
$CM --build build-glslang-win > build-glslang-win.log 2>&1 || { grep -E "error" build-glslang-win.log | head; exit 1; }
$CM --install build-glslang-win > /dev/null
ls sysroot-win/bin sysroot-win/lib
