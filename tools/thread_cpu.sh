#!/bin/bash
# CPU por hilo de la app durante N segundos (por defecto 30), desde /proc (utime+stime, en ms): para comparar
# cuánto trabajo hace cada hilo con y sin un cambio, aunque los fps no cambien.
# uso: tools/thread_cpu.sh [segundos]
export MSYS_NO_PATHCONV=1
S=${HV_ADB_SERIAL:-$(adb devices | awk 'NR>1 && $2=="device"{print $1; exit}')}
T=${1:-30}
adb -s $S shell "P=\$(pidof com.hackversus.port); for t in /proc/\$P/task/*; do echo \$(basename \$t) \$(cat \$t/comm | tr ' ' _) \$(cut -d' ' -f14,15 \$t/stat); done; sleep $T; echo ---; for t in /proc/\$P/task/*; do echo \$(basename \$t) \$(cat \$t/comm | tr ' ' _) \$(cut -d' ' -f14,15 \$t/stat); done" | awk -v T=$T '
/^---/ {after=1; next}
{ k=$1" "$2; v=$3+$4; if (!after) a[k]=v; else if (k in a) d[k]=v-a[k] }
END { for (k in d) if (d[k] > 0) { tot+=d[k]; printf "%8.1f ms/s  %s\n", d[k]*10/T, k } ; printf "%8.1f ms/s  TOTAL\n", tot*10/T }' | sort -rn | head -16
