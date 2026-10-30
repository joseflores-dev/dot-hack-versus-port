#!/bin/bash
# Abre la app (VAR=valor opcionales en /sdcard/PS3/hv_env.txt) y espera al menú de título (.hack//Versus:
# STORY MODE / VERSUS MODE / OPTION). Con partida guardada el juego llega solo; sin ella pasa por la pantalla
# Stereoscopic (los monos) y los avisos de guardado, que este script atraviesa (○, ✕, ✕).
# uso: tools/goto_title.sh <captura.png> [VAR=valor ...]
export MSYS_NO_PATHCONV=1
S=${HV_ADB_SERIAL:-$(adb devices | awk 'NR>1 && $2=="device"{print $1; exit}')}
OUT="$1"; shift
D="$(dirname "$0")"
TMP="$D/../build-apk/goto_tmp.png"
# Datos de la app en modo ISO (antes /sdcard/PS3).
A=/sdcard/Android/data/com.hackversus.port/files/PS3
# Sin partida guardada, el salto automático (✕ cada 1,5 s) atraviesa la intro hasta los monos. CON partida el juego
# llega solo al título y esas ✕ entrarían a STORY MODE (el hub ALTIMIT): no activarlo.
if adb -s $S shell "ls $A/dev_hdd0/home/00000001/savedata/BLJS93008-HACK_VERSUS/VERSUS.S" 2>/dev/null | grep -q VERSUS.S; then ENV=""; else ENV="HV_AUTOSKIP=1\n"; fi
# Mientras trabajamos: sin logos de arranque (HV_INTRO=0,17: carga y siguiente escena); una HV_INTRO=... la reemplaza.
case " $* " in *" HV_INTRO="*) ;; *) ENV="${ENV}HV_INTRO=0,17\n" ;; esac
for kv in "$@"; do ENV="$ENV$kv\n"; done
adb -s $S shell "printf '$ENV' > $A/hv_env.txt; am force-stop com.hackversus.port; am start -n com.hackversus.port/.SetupActivity" >/dev/null
sleep 12
for i in $(seq 1 60); do
    adb -s $S exec-out screencap -p > "$TMP"
    st=$(python -I -c "
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert('RGB')
ok = lambda p, c, t=16: all(abs(a - b) <= t for a, b in zip(im.getpixel(p), c))
if ok((560, 250), (255, 255, 255)) and ok((1000, 250), (255, 255, 255)) and ok((1400, 650), (55, 96, 106), 22): print('title')
elif ok((300, 180), (11, 75, 106)) and ok((600, 178), (11, 75, 106)) and ok((200, 231), (9, 30, 50)): print('monos')
else: print('?')" "$TMP")
    if [ "$st" = title ]; then cp "$TMP" "$OUT"; echo "título tras $((12 + 2 * i)) s aprox."; exit 0; fi
    if [ "$st" = monos ]; then sleep 3; bash "$D/pad.sh" o:4 x:4 x:5; fi
    sleep 2
done
cp "$TMP" "$OUT"; echo "no se detectó el título"; exit 1
