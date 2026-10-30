#!/bin/bash
# A/B en frío: alterna mediciones de pelea entre variantes, esperando a que la piel baje de HV_COOL (35 °C) antes de cada una.
# uso: tools/ab_fight.sh <rondas> "<etiqueta>:VAR=val VAR=val" "<etiqueta>:..."   (una variante sin variables: "base:")
export MSYS_NO_PATHCONV=1
S=${HV_ADB_SERIAL:-$(adb devices | awk 'NR>1 && $2=="device"{print $1; exit}')}
D="$(dirname "$0")"
R=$1; shift
for r in $(seq 1 $R); do
    for v in "$@"; do
        tag=${v%%:*}; vars=${v#*:}
        adb -s $S shell "am force-stop com.hackversus.port" >/dev/null
        for i in $(seq 1 90); do
            t=$(adb -s $S shell "dumpsys thermalservice | grep mName=SKIN | head -1" | sed 's/.*mValue=\([0-9.]*\).*/\1/')
            [ "${t%.*}" -lt "${HV_COOL:-35}" ] && break; sleep 10
        done
        adb -s $S shell "input keyevent 224" >/dev/null   # solo despierta si se apagó sola; nunca bloquear (pedido del usuario)
        echo "$(bash "$D/measure_fight.sh" "${tag}_r$r" $vars | tail -1)  [piel inicial $t °C]"
    done
done
adb -s $S shell "am force-stop com.hackversus.port" >/dev/null
