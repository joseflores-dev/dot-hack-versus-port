#!/bin/bash
# Abre la app (VAR=valor opcionales en hv_env.txt) y llega a una pelea Versus > Offline > CPU VS CPU (Sora vs Sora,
# Aura's Garden). Deja la captura de la pelea en el archivo indicado. Navegador por estados: tools/nav_fight.py.
# HV_P1_PRE: teclas en el carrusel antes de elegir al jugador 1 (Black Rose: "right:0.6 right:0.6 right:0.6 right:0.6");
# HV_P1_MENU: teclas en su menú después de elegirlo, terminando con el cursor en Start Battle (color 3:
# "down:1 down:1 x:1 right:1 right:1 x:1 up:1 up:1").
# uso: tools/goto_fight.sh <captura.png> [VAR=valor ...]
OUT="$1"; shift
python -I "$(dirname "$0")/nav_fight.py" --shot "$OUT" "$@"
