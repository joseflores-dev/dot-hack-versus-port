#!/bin/bash
# measure_fight.sh + cuántos cuadros de la pelea pasaron de 37 ms (histograma "frame ms" de las mismas 6 líneas).
# uso: tools/measure_slow.sh <etiqueta> [VAR=valor ...]
export MSYS_NO_PATHCONV=1
S=adb-R5CT61Z9HZA-breWSf._adb-tls-connect._tcp
L=/sdcard/Android/data/com.hackversus.port/files/log.txt
D="$(dirname "$0")"
r=$(bash "$D/measure_fight.sh" "$@" 2>&1 | tail -1)
slow=$(adb -s $S shell "grep 'frame ms' $L | tail -6" | awk '{for(i=1;i<=NF;i++){if($i=="37-45")a+=$(i+1);if($i=="45-55")b+=$(i+1);if($i==">55")c+=$(i+1);if($i=="30-37")o+=$(i+1)}} END{printf "lentos(>=37ms) %d  [37-45 %d, 45-55 %d, >55 %d]  a tiempo(30-37) %d", a+b+c, a, b, c, o}')
echo "$r | $slow"
