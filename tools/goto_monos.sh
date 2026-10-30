#!/bin/bash
# Abre la app (con las VAR=valor dadas en /sdcard/PS3/hv_env.txt) y espera a que su salto de intro (HV_AUTOSKIP,
# hackversus/stubs.cpp) llegue a la pantalla Stereoscopic (los monos morados). Deja la captura en el archivo indicado.
# uso: tools/goto_monos.sh <captura.png> [VAR=valor ...]
export MSYS_NO_PATHCONV=1
S=${HV_ADB_SERIAL:-$(adb devices | awk 'NR>1 && $2=="device"{print $1; exit}')}
OUT="$1"; shift
TMP="$(dirname "$0")/../build-apk/goto_tmp.png"
ENV=""; for kv in "$@"; do ENV="$ENV$kv\n"; done
adb -s $S shell "printf '$ENV' > /sdcard/PS3/hv_env.txt; am force-stop com.hackversus.port; am start -n com.hackversus.port/.HackVersusActivity" >/dev/null
sleep 12
for i in $(seq 1 70); do
    adb -s $S exec-out screencap -p > "$TMP"
    # Cabecera del panel Stereoscopic (~11,75,106) a lo ancho y el fondo oscuro del panel (~9,30,50)
    # (coordenadas de pantalla completa): ya estamos en los monos.
    if python -I -c "
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert('RGB')
ok = lambda p, c: all(abs(a - b) <= 14 for a, b in zip(im.getpixel(p), c))
sys.exit(0 if ok((300, 180), (11, 75, 106)) and ok((600, 178), (11, 75, 106)) and ok((200, 231), (9, 30, 50)) else 1)" "$TMP"; then
        sleep 4   # que la escena termine de cargar los modelos
        adb -s $S exec-out screencap -p > "$OUT"
        echo "en los monos tras $((12 + 2 * i)) s aprox."
        exit 0
    fi
    sleep 2
done
adb -s $S exec-out screencap -p > "$OUT"
echo "no se detectó la escena"; exit 1
