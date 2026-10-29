#!/bin/bash
# Arma el APK del port: libmain.so (juego recompilado, HV_APK) + libSDL2.so + SDLActivity.
# Correr desde Git Bash:  tools/build_apk.sh      -> out/hackversus.apk
set -e
# Las rutas POSIX ($TC, $P) dependen de la conversión de MSYS: un MSYS_NO_PATHCONV=1 heredado del shell rompe la configuración.
unset MSYS_NO_PATHCONV
cd "$(dirname "$0")/.."
P=$PWD; W=$(pwd -W)
TC=$P/toolchain
NDK=$TC/android-ndk-r30
BT=$TC/build-tools-35.0.1
AJ=$TC/android-35/android.jar
export JAVA_HOME=$(cd $TC/jdk-17* && pwd)
export PATH="$JAVA_HOME/bin:$PATH"
CM=$TC/cmake-4.4.5-windows-x86_64/bin/cmake
# HV_BUILD_DIR (default build-apk), HV_PGO=gen (instrumented, writes a profile) or HV_PGO=use:<file.profdata>.
BD=${HV_BUILD_DIR:-build-apk}
PGOF=""
case "${HV_PGO:-}" in
    gen)   PGOF="-fprofile-generate" ;;
    use:*) PGOF="-fprofile-use=$(cygpath -m "${HV_PGO#use:}") -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date -Wno-backend-plugin" ;;
esac
# HV_LTO=1: ThinLTO across the runtime and the 64 translated PPU files (combines with HV_PGO).
[ "${HV_LTO:-0}" = 1 ] && PGOF="$PGOF -flto=thin"
# HV_SPU_FAST=1: SPU float ops in round-toward-zero mode as the bare NEON instruction (SPU_RZ_FAST, spu_helpers.h).
[ "${HV_SPU_FAST:-0}" = 1 ] && PGOF="$PGOF -DSPU_RZ_FAST"
OUT=$P/out; B=$P/build-apk-java
mkdir -p "$OUT"; rm -rf "$B"; mkdir -p "$B/classes" "$B/dex" "$B/res"

# 1. libmain.so
IN="$W/game/EBOOT.ELF"; for e in spu_jobs/*.elf; do IN="$IN;$W/$e"; done
# Always reconfigure (cheap): picks up new jobs in spu_jobs/ for PORT_SPU_INPUTS.
if true; then
    $CM -S hackversus -B $BD -G Ninja -DCMAKE_MAKE_PROGRAM=$TC/ninja.exe \
        -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake -DANDROID_ABI=arm64-v8a \
        -DANDROID_PLATFORM=android-31 -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_PREFIX_PATH=$P/sysroot -DCMAKE_FIND_ROOT_PATH=$P/sysroot -DPS3RECOMP_RSX_VULKAN=ON \
        -DPS3RECOMP_DIR=$P/ps3recomp-0.18.0 -DRECOMP_DIR=$P/recomp -DPython3_EXECUTABLE=$P/.venv/Scripts/python.exe \
        "-DPORT_SPU_INPUTS=$IN" -Dglslang_DIR=$W/sysroot/lib/cmake/glslang -DPS3RECOMP_SPU_MERGE_CHUNKS=ON -DHV_APK=ON \
        "-DCMAKE_C_FLAGS=$PGOF" "-DCMAKE_CXX_FLAGS=$PGOF" "-DCMAKE_SHARED_LINKER_FLAGS=$PGOF" > $BD-configure.log
fi
# Fase 19: el contexto SPU tampoco vive en el LS ni en memoria del juego -> __restrict (el lifter no lo emite).
sed -i 's/(spu_context\* ctx)/(spu_context* __restrict ctx)/g' $BD/generated_spu/*/spu_recomp.c $BD/generated_spu/*/spu_recomp.h
$CM --build $BD > $BD/build.log 2>&1 || { grep -E "error" $BD/build.log | head -20; exit 1; }
mkdir -p "$B/lib/arm64-v8a"
$NDK/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-strip -o "$B/lib/arm64-v8a/libmain.so" $BD/libmain.so
cp sysroot/lib/libSDL2.so "$B/lib/arm64-v8a/"

# 2. Java -> dex
BCP="$(cygpath -w "$AJ");$(cygpath -w "$BT/core-lambda-stubs.jar")"   # a ;-list is not path-converted
javac -encoding UTF-8 -nowarn -source 8 -target 8 -bootclasspath "$BCP" -d "$B/classes" \
    $(find apk/java -name '*.java') 2> "$B/javac.log" || { cat "$B/javac.log"; exit 1; }
"$BT/d8.bat" --release --min-api 31 --lib "$AJ" --output "$B/dex" $(find "$B/classes" -name '*.class')

# 3. resources + manifest
"$BT/aapt2.exe" compile --dir apk/res -o "$B/res/res.zip"
# The decrypted EBOOT ships as an asset: the app unpacks it once into its files dir (main.cpp), so the player
# only has to provide the disc image.
mkdir -p "$B/assets" && cp game/EBOOT.ELF "$B/assets/EBOOT.ELF"
"$BT/aapt2.exe" link -I "$AJ" --manifest apk/AndroidManifest.xml -A "$B/assets" -o "$B/base.apk" "$B/res/res.zip"

# 4. dex + libs into the APK, align, sign
python -I - "$B" <<'EOF'
import os, sys, zipfile
b = sys.argv[1]
with zipfile.ZipFile(os.path.join(b, 'base.apk'), 'a', zipfile.ZIP_DEFLATED) as z:
    z.write(os.path.join(b, 'dex', 'classes.dex'), 'classes.dex')
    for so in ('libmain.so', 'libSDL2.so'):
        z.write(os.path.join(b, 'lib', 'arm64-v8a', so), 'lib/arm64-v8a/' + so)
EOF
"$BT/zipalign.exe" -f -p 4 "$B/base.apk" "$B/aligned.apk"
KS=$P/apk/debug.keystore
[ -f "$KS" ] || keytool -genkeypair -keystore "$KS" -storepass android -keypass android -alias hv \
    -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=hackversus port" > /dev/null
"$BT/apksigner.bat" sign --ks "$KS" --ks-pass pass:android --key-pass pass:android --out "$OUT/hackversus.apk" "$B/aligned.apk"
ls -l "$OUT/hackversus.apk"
