#!/bin/bash
# Llega a la pelea CPU vs CPU (vía goto_title) con las VAR=valor dadas y promedia los fps de 30 s allí (6 líneas de 5 s).
# uso: tools/measure_fight.sh <etiqueta> [VAR=valor ...]
export MSYS_NO_PATHCONV=1
S=${HV_ADB_SERIAL:-$(adb devices | awk 'NR>1 && $2=="device"{print $1; exit}')}
L=/sdcard/Android/data/com.hackversus.port/files/log.txt
TAG="$1"; shift
D="$(dirname "$0")/.."
bash "$D/tools/goto_fight.sh" "$D/frames/f_$TAG.png" "$@" >/dev/null || { echo "$TAG: no llegó"; exit 1; }
N0=$(adb -s $S shell "grep -c ' fps' $L")
until [ "$(adb -s $S shell "grep -c ' fps' $L")" -ge $((N0 + 7)) ]; do sleep 3; done
adb -s $S shell "grep ' fps' $L | tail -6" | awk -v t="$TAG" '{s+=$3; w+=$(NF-5); v=v" "$3} END{printf "%s: %.1f fps (%s ) espera GPU %.0f%%\n", t, s/NR, v, w/NR}'
# Cierra la app al terminar (si no, la pelea sigue corriendo y calienta el teléfono entre mediciones); HV_KEEP=1 la deja.
[ "${HV_KEEP:-0}" = 1 ] || adb -s $S shell "am force-stop com.hackversus.port; grep -A10 ' fps,' $L | tail -11" > "$D/frames/m_$TAG.log"
