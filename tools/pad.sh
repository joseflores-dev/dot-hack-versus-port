#!/bin/bash
# Pulsa botones del pad táctil de la app y opcionalmente guarda una captura.
# uso: tools/pad.sh <boton>[:espera_s] ... [-- captura.png]
# botones: x o t(riángulo) s(cuadrado) up down left right start select l1 r1 l2 r2
export MSYS_NO_PATHCONV=1
S=${HV_ADB_SERIAL:-$(adb devices | awk 'NR>1 && $2=="device"{print $1; exit}')}
declare -A P=([x]="1350 590" [o]="1450 476" [t]="1350 360" [s]="1250 476" [up]="200 405" [down]="200 577"
              [left]="113 490" [right]="287 490" [start]="880 662" [select]="664 662"
              [l1]="116 172" [r1]="1428 172" [l2]="116 57" [r2]="1428 57")
while [ $# -gt 0 ]; do
    if [ "$1" = "--" ]; then shift; sleep 1; adb -s $S exec-out screencap -p > "$1"; exit 0; fi
    b="${1%%:*}"; w=1.2; [[ "$1" == *:* ]] && w="${1##*:}"
    adb -s $S shell input swipe ${P[$b]} ${P[$b]} 120
    sleep "$w"; shift
done
