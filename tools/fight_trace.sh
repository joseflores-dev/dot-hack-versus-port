#!/bin/bash
# Línea de tiempo PS3_FTRACE de una pelea CPU vs CPU en el teléfono y su informe por cuadro.
# uso: tools/fight_trace.sh <etiqueta> [VAR=valor ...]   -> frames/ft_<etiqueta>.txt y frames/ft_<etiqueta>_report.txt
# La traza empieza a los 150 s del arranque de la app (la pelea llega a ~125 s) y dura 5 s.
export MSYS_NO_PATHCONV=1
S=${HV_ADB_SERIAL:-$(adb devices | awk 'NR>1 && $2=="device"{print $1; exit}')}
TAG="$1"; shift
D="$(dirname "$0")/.."; W=$(cd "$D" && pwd -W)
F=/sdcard/Android/data/com.hackversus.port/files/ft.txt
adb -s $S shell "rm -f $F"
bash "$D/tools/goto_fight.sh" "$D/frames/ft_$TAG.png" "PS3_FTRACE=150:5:$F" "$@" >/dev/null || { echo "$TAG: no llegó"; exit 1; }
# El archivo se escribe al terminar la ventana; esperar a que exista y deje de crecer.
prev=-1
for i in $(seq 1 30); do
    n=$(adb -s $S shell "stat -c %s $F 2>/dev/null" | tr -dc 0-9)
    [ -n "$n" ] && [ "$n" -gt 0 ] && [ "$n" = "$prev" ] && break
    prev=${n:--1}; sleep 5
done
adb -s $S pull $F "$D/frames/ft_$TAG.txt" >/dev/null
adb -s $S shell am force-stop com.hackversus.port
python -I "$W/ps3recomp-0.18.0/tools/ftrace_report.py" "$W/frames/ft_$TAG.txt" --frames 3 > "$D/frames/ft_${TAG}_report.txt"
echo "$TAG: $(wc -l < "$D/frames/ft_$TAG.txt") spans -> frames/ft_${TAG}_report.txt"
