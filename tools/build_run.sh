#!/bin/bash
# Compila el port para Android, lo sube al S22 y lo corre N segundos.
# uso: tools/build_run.sh [segundos] [VAR=valor ...]   (log en build-game/run.log)
set -e
cd "$(dirname "$0")/.."
export MSYS_NO_PATHCONV=1
S=adb-R5CT61Z9HZA-breWSf._adb-tls-connect._tcp
T=${1:-40}; shift || true
toolchain/cmake-4.4.5-windows-x86_64/bin/cmake --build build-game > build-game/build.log 2>&1 || { grep -E "error:" build-game/build.log | head -20; exit 1; }
toolchain/android-ndk-r30/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-strip -o build-game/hackversus build-game/MyGameRecomp
adb -s $S push build-game/hackversus /data/local/tmp/ps3r/ >/dev/null
adb -s $S shell "cd /data/local/tmp/ps3r && chmod 755 hackversus && $* PS3_VFS_ROOT=/sdcard/PS3/vfs LD_LIBRARY_PATH=. timeout $T ./hackversus EBOOT.ELF /sdcard/PS3/vfs > run.log 2>&1; echo exit=\$?"
adb -s $S pull /data/local/tmp/ps3r/run.log build-game/run.log >/dev/null
wc -l build-game/run.log
