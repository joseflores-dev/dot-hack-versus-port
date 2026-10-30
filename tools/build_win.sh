#!/bin/bash
# Compila el port para Windows x64 (llvm-mingw, Vulkan vía SDL2) -> out-win/hackversus.exe
# Correr desde Git Bash:  tools/build_win.sh   (antes, una vez: tools/build_win_deps.sh)
set -e
unset MSYS_NO_PATHCONV
cd "$(dirname "$0")/.."
P=$PWD; W=$(pwd -W)
TC=$P/toolchain
MG=$(cd $TC/llvm-mingw-* && pwd)
export PATH="$MG/bin:$PATH"
CM=$TC/cmake-4.4.5-windows-x86_64/bin/cmake
B=build-win

IN="$W/game/EBOOT.ELF"; for e in spu_jobs/*.elf; do IN="$IN;$W/$e"; done
# Ganchos del port en el código generado (mods de personajes): recomp/ está fuera del repo.
python -I tools/apply_recomp_hooks.py recomp || exit 1
$CM -S hackversus -B $B -G Ninja -DCMAKE_MAKE_PROGRAM=$TC/ninja.exe -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=$MG/bin/x86_64-w64-mingw32-clang.exe -DCMAKE_CXX_COMPILER=$MG/bin/x86_64-w64-mingw32-clang++.exe \
    -DCMAKE_RC_COMPILER=$MG/bin/x86_64-w64-mingw32-windres.exe \
    -DCMAKE_PREFIX_PATH=$W/sysroot-win -DPS3RECOMP_RSX_VULKAN=ON -DPS3RECOMP_VULKAN_INCLUDE_DIR=$W/sysroot-win/vkinclude \
    -DPS3RECOMP_DIR=$W/ps3recomp-0.18.0 -DRECOMP_DIR=$W/recomp -DPython3_EXECUTABLE=$W/.venv/Scripts/python.exe \
    "-DPORT_SPU_INPUTS=$IN" -Dglslang_DIR=$W/sysroot-win/lib/cmake/glslang -DSDL2_DIR=$W/sysroot-win/lib/cmake/SDL2 \
    -DPS3RECOMP_SPU_MERGE_CHUNKS=ON > $B-configure.log
# Fase 19: el contexto SPU no vive en memoria del juego -> __restrict (el lifter no lo emite).
sed -i 's/(spu_context\* ctx)/(spu_context* __restrict ctx)/g' $B/generated_spu/*/spu_recomp.c $B/generated_spu/*/spu_recomp.h 2>/dev/null || true
$CM --build $B -- -k 0 > $B/build.log 2>&1 || { grep -E "error" $B/build.log | head -40; exit 1; }
mkdir -p out-win
cp $B/MyGameRecomp.exe out-win/hackversus.exe
cp sysroot-win/bin/SDL2.dll out-win/
ls -l out-win
