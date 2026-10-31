#!/bin/bash
# Abre la app (VAR=valor opcionales en hv_env.txt), llega a los monos y navega a una pelea
# Versus > Offline > CPU VS CPU (Sora vs Sora, Aura's Garden). Deja la captura en el archivo indicado.
# uso: tools/goto_fight.sh <captura.png> [VAR=valor ...]
D="$(dirname "$0")"
OUT="$1"; shift
T0=$(date +%s)
bash "$D/goto_title.sh" "$D/../frames/fight_title.png" "$@" >/dev/null || { echo "no llegó al título"; exit 1; }
# título -> Versus -> Offline -> CPU VS CPU (el menú del título ignora el pad durante su fundido: esperar antes del primer "abajo")
sleep 3
bash "$D/pad.sh" down:1.5 x:4 x:5 down:1.2 down:1.2 x:7
# personaje P1, P2, menú "Start Battle", escenario, "Yes", carga
# HV_P1_PRE: teclas en el carrusel antes de elegir al jugador 1 (Black Rose: "right:0.6 right:0.6 right:0.6 right:0.6");
# HV_P1_MENU: teclas en su menú después de elegirlo, terminando en Start Battle (color 3:
# "down:1 down:1 x:1 right:1 right:1 x:1 up:1 up:1")
bash "$D/pad.sh" ${HV_P1_PRE:-} x:2 ${HV_P1_MENU:-} x:2.5 x:2.5 x:3 x:2.5 x:30 -- "$OUT"   # P1, P2, Start Battle, escenario, Yes, carga
echo "pelea tras $(( $(date +%s) - T0 )) s"
