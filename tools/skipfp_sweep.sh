#!/bin/bash
# Para cada hash de fragment program: abre la app con HV_SKIP_FP=<hash>, aprieta X hasta los monos y captura.
export MSYS_NO_PATHCONV=1
S=${HV_ADB_SERIAL:-$(adb devices | awk 'NR>1 && $2=="device"{print $1; exit}')}
D="$(dirname "$0")/../frames/skipfp"; mkdir -p "$D"
for h in "$@"; do
    adb -s $S shell "echo HV_SKIP_FP=$h > /sdcard/PS3/hv_env.txt; am force-stop com.hackversus.port; am start -n com.hackversus.port/.HackVersusActivity" >/dev/null
    sleep 15
    adb -s $S shell 'for i in $(seq 1 45); do input swipe 1350 590 1350 590 300; sleep 3; done'
    adb -s $S exec-out screencap -p > "$D/$h.png"
    echo "done $h"
done
adb -s $S shell 'rm -f /sdcard/PS3/hv_env.txt'
