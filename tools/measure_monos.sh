#!/bin/bash
# Llega a los monos con las VAR=valor dadas y promedia los fps de 30 s allí (6 líneas de 5 s).
# uso: tools/measure_monos.sh <etiqueta> [VAR=valor ...]
export MSYS_NO_PATHCONV=1
S=${HV_ADB_SERIAL:-$(adb devices | awk 'NR>1 && $2=="device"{print $1; exit}')}
L=/sdcard/Android/data/com.hackversus.port/files/log.txt
TAG="$1"; shift
D="$(dirname "$0")/.."
bash "$D/tools/goto_monos.sh" "$D/frames/m_$TAG.png" "$@" >/dev/null || { echo "$TAG: no llegó"; exit 1; }
N0=$(adb -s $S shell "grep -c ' fps' $L")
until [ "$(adb -s $S shell "grep -c ' fps' $L")" -ge $((N0 + 7)) ]; do sleep 3; done
adb -s $S shell "grep ' fps' $L | tail -6" | awk -v t="$TAG" '{s+=$3; w+=$(NF-5); v=v" "$3} END{printf "%s: %.1f fps (%s ) espera GPU %.0f%%\n", t, s/NR, v, w/NR}'
