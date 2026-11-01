#!/bin/bash
# Abre la app (VAR=valor opcionales en hv_env.txt) y espera al menú de título (.hack//Versus: STORY MODE /
# VERSUS MODE / OPTION), atravesando el aviso de autoguardado y la pantalla Stereoscopic (los monos) si aparecen.
# Navegador por estados: tools/nav_fight.py (reconoce cada pantalla y repite la pulsación si el juego no la tomó).
# uso: tools/goto_title.sh <captura.png> [VAR=valor ...]
OUT="$1"; shift
python -I "$(dirname "$0")/nav_fight.py" --title --shot "$OUT" "$@"
