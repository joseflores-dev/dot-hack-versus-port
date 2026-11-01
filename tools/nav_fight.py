"""Navegador por estados del teléfono: abre la app y llega al título o a una pelea CPU VS CPU (Sora vs Sora,
Aura's Garden) sin esperas fijas. En cada vuelta captura la pantalla, reconoce dónde está y pulsa lo que toca; si
la pantalla no cambia tras una pulsación (el juego ignora el pad durante fundidos y cargas), la repite.

Pantallas: por parecido con las capturas de tools/nav_ref/ (imagen reducida a 96x45; capturas propias del teléfono a
1544x720, una por pantalla, nombradas como las claves de ACTION; no van al repo); el cursor del título y del
menú VERSUS por la zona del menú a resolución completa; la pelea por las dos barras de vida naranjas (fila y=84).
uso: python tools/nav_fight.py [--title] [--shot captura.png] [--p1-pre "right:0.6 ..."] [--p1-menu "down:1 x:1 ..."]
                               [--timeout 180] [VAR=valor ...]
     (--p1-pre/--p1-menu también salen de HV_P1_PRE / HV_P1_MENU; VAR=valor van a hv_env.txt)
Salida: una línea "título|pelea tras N s" (código 0) o "no llegó: <estado>" (código 1, con captura en --shot).
"""
import glob
import os
import subprocess
import sys
import time

import numpy as np
from PIL import Image
import io

HERE = os.path.dirname(os.path.abspath(__file__))
PKG = "com.hackversus.port"
FILES = "/sdcard/Android/data/com.hackversus.port/files"
A = FILES + "/PS3"
PAD = {"x": (1350, 590), "o": (1450, 476), "t": (1350, 360), "s": (1250, 476), "up": (200, 405), "down": (200, 577),
       "left": (113, 490), "right": (287, 490), "start": (880, 662), "select": (664, 662),
       "l1": (116, 172), "r1": (1428, 172), "l2": (116, 57), "r2": (1428, 57)}
W, H = 1544, 720
# zonas del menú (coordenadas de 1544x720) para el cursor
ROI_TITLE = (520, 330, 1000, 480)
ROI_VS = (470, 330, 1200, 480)

ENV = dict(os.environ, MSYS_NO_PATHCONV="1")
SERIAL = os.environ.get("HV_ADB_SERIAL")


def adb(*args, out=False):
    cmd = ["adb"] + (["-s", SERIAL] if SERIAL else []) + list(args)
    r = subprocess.run(cmd, capture_output=True, env=ENV)
    return r.stdout if out else r.returncode


def shell(cmd):
    return adb("shell", cmd, out=True).decode("utf-8", "replace")


def grab():
    data = adb("exec-out", "screencap", "-p", out=True)
    try:
        im = Image.open(io.BytesIO(data)).convert("RGB")
    except Exception:
        return None
    return im if im.size == (W, H) else im.resize((W, H))


def press(b):
    x, y = PAD[b]
    adb("shell", f"input swipe {x} {y} {x} {y} 120")


def keys(seq):
    """'right:0.6 x:1 ...' como pad.sh: botón y espera después."""
    for k in seq.split():
        b, _, w = k.partition(":")
        press(b)
        time.sleep(float(w) if w else 1.2)


def small(im):
    return np.asarray(im.resize((96, 45), Image.BILINEAR), dtype=np.float32)


def crop(im, roi):
    return np.asarray(im.crop(roi), dtype=np.float32)


REFS = {}
for f in glob.glob(os.path.join(HERE, "nav_ref", "*.png")):
    k = os.path.basename(f)[:-4]
    im = Image.open(f).convert("RGB")
    REFS[k] = (small(im), im)
FINE_TITLE = {k: crop(REFS[k][1], ROI_TITLE) for k in ("title_story", "title_versus")}
FINE_VS = {k: crop(REFS[k][1], ROI_VS) for k in ("vs_offline", "vs_1pcpu", "vs_2p", "vs_cpucpu")}


def is_fight(im):
    a = np.asarray(im, dtype=np.int32)
    for y in range(80, 93, 3):
        row = a[y]
        o = lambda s: int(((s[:, 0] > 180) & (s[:, 2] < 90) & (s[:, 1] > 30)).sum())
        l, r = o(row[230:680]), o(row[860:1310])
        if l + r >= 120 and min(l, r) >= 5:
            return True
    return False


# Selección de personaje y de escenario: el fondo animado aleja la imagen completa de la referencia, así que van por
# zonas: el encabezado ("CHARACTER SELECT" / "STAGE SELECT"; ~5-14 contra su referencia, ~35 contra la otra) y los
# recuadros de menú de P1 y P2 (~13 contra su referencia, >40 sin recuadro). P1/P2 sin menú: por orden (main).
HDR, POP1, POP2 = (20, 0, 760, 50), (112, 222, 572, 362), (968, 222, 1420, 372)
CHAR_HDR = [crop(REFS[k][1], HDR) for k in ("char_p1", "char_p1menu", "char_p2", "char_p2menu")]
STAGE_HDR = crop(REFS["stage"][1], HDR)
P1MENU = crop(REFS["char_p1menu"][1], POP1)
P2MENU = crop(REFS["char_p2menu"][1], POP2)


def dist(a, b):
    return float(np.abs(a - b).mean())


def classify(im):
    if is_fight(im):
        return "fight"
    s = small(im)
    d = sorted((float(np.abs(s - r).mean()), k) for k, (r, _) in REFS.items()
               if not k.startswith(("fight", "char", "stage")))
    best, k = d[0]
    if os.environ.get("HV_NAV_DEBUG"):
        classify.last = f"{k}_{best:.1f}"
    if best > 6.0:
        h = crop(im, HDR)
        dc, ds = min(dist(h, r) for r in CHAR_HDR), dist(h, STAGE_HDR)
        if ds < 20 and dc > 25:
            return "stage"
        if dc < 20 and ds > 25:
            if dist(crop(im, POP1), P1MENU) < 25:
                return "char_p1menu"
            if dist(crop(im, POP2), P2MENU) < 25:
                return "char_p2menu"
            return "char"
        return "?"
    if k.startswith("title"):
        c = crop(im, ROI_TITLE)
        dd = {kk: float(np.abs(c - r).mean()) for kk, r in FINE_TITLE.items()}
        k = min(dd, key=dd.get)
        # ni STORY ni VERSUS resaltado de forma clara: OPTION (o el fundido)
        if dd[k] > 12.0:
            return "title_other"
    elif k.startswith("vs_"):
        c = crop(im, ROI_VS)
        dd = {kk: float(np.abs(c - r).mean()) for kk, r in FINE_VS.items()}
        k = min(dd, key=dd.get)
    return k


# qué pulsar en cada pantalla
ACTION = {"autosave": "x", "monos": "o", "title_story": "down", "title_other": "up", "title_versus": "x",
          "vs_offline": "x", "vs_1pcpu": "down", "vs_2p": "down", "vs_cpucpu": "x",
          "char_p1": "x", "char_p1menu": "x", "char_p2": "x", "char_p2menu": "x", "stage": "x", "confirm": "x"}


def main():
    args = sys.argv[1:]
    title_only = False
    shot = None
    p1_pre = os.environ.get("HV_P1_PRE", "")
    p1_menu = os.environ.get("HV_P1_MENU", "")
    timeout = 180.0
    kv = []
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--title":
            title_only = True
        elif a == "--shot":
            i += 1; shot = args[i]
        elif a == "--p1-pre":
            i += 1; p1_pre = args[i]
        elif a == "--p1-menu":
            i += 1; p1_menu = args[i]
        elif a == "--timeout":
            i += 1; timeout = float(args[i])
        else:
            kv.append(a)
        i += 1

    # hv_env.txt: sin partida guardada, el salto automático atraviesa la intro hasta los monos; con partida no
    # (sus ✕ entrarían a STORY MODE). Sin logos mientras trabajamos, salvo HV_INTRO=... explícito.
    env = []
    if "VERSUS.S" not in shell(f"ls {A}/dev_hdd0/home/00000001/savedata/BLJS93008-HACK_VERSUS/VERSUS.S 2>/dev/null"):
        env.append("HV_AUTOSKIP=1")
    if not any(x.startswith("HV_INTRO=") for x in kv):
        env.append("HV_INTRO=0,17")
    env += kv
    body = "".join(e + "\\n" for e in env)
    shell(f"printf '{body}' > {A}/hv_env.txt; am force-stop {PKG}; input keyevent 224; "
          f"am start -n {PKG}/.SetupActivity >/dev/null")
    t0 = time.time()
    last_state, last_press, same_since = None, 0.0, t0
    done_pre = done_menu = seen_p1menu = seen_p2menu = confirmed = False
    im = None
    hist = []
    while time.time() - t0 < timeout:
        im = grab()
        if im is None:
            time.sleep(0.5)
            continue
        st = classify(im)
        if time.time() - t0 < 3 and st in ("fight",) or (time.time() - t0 < 3 and st.startswith("title")):
            st = "?"   # restos de la sesión anterior: la app nueva tarda > 5 s en mostrar algo
        # P1/P2 sin menú por orden; después del menú de P2 la selección (y la pantalla "VS" de la carga, que tiene
        # el mismo encabezado) es transición: una ✕ ahí elegiría el escenario a ciegas
        if st == "char":
            st = "loading" if seen_p2menu else ("char_p2" if seen_p1menu else "char_p1")
        elif st == "char_p1menu":
            seen_p1menu = True
        elif st == "char_p2menu":
            seen_p2menu = True
        now = time.time()
        # tras confirmar la pelea solo se espera la carga (la pantalla de carga se parece a la selección):
        # se repite la ✕ únicamente si la pregunta sigue en pantalla
        if confirmed and st not in ("fight", "confirm"):
            st = "loading"
        if st != last_state:
            hist.append(f"{st}@{now - t0:.0f}")
            last_state, same_since = st, now
        # la primera captura puede ser de la sesión anterior (la app tarda en cerrarse): la pelea solo cuenta
        # después de haber pasado por la selección
        if st == "fight" and not (seen_p2menu or confirmed):
            st = "?"
        if st == "fight" or (title_only and st.startswith("title")):
            if title_only:
                time.sleep(1.0)   # el menú ignora el pad durante su fundido
            if shot:
                im.save(shot)
            print(f"{'pelea' if st == 'fight' else 'título'} tras {now - t0:.0f} s")
            if os.environ.get("HV_NAV_LOG"):
                print(" ".join(hist))
            return 0
        if os.environ.get("HV_NAV_DEBUG"):
            im.resize((386, 180)).save(f"{os.environ['HV_NAV_DEBUG']}/q_{now - t0:05.1f}_{st.replace('?', 'q')}_{classify.last}.png")
        if st in ("?", "loading"):
            if now - t0 > 15 and not shell(f"pidof {PKG}").strip():
                hist.append("app cerrada")
                break
            continue
        # una pulsación por pantalla; si en 2,5 s no cambió, se repite (el juego no la tomó)
        if last_press > same_since and now - last_press < 2.5:
            continue
        # OPTION resaltado o el fundido del título: solo si se mantiene
        if st == "title_other" and now - same_since < 1.5:
            continue
        if st == "char_p1" and p1_pre and not done_pre:
            keys(p1_pre)
            done_pre = True
        if st == "char_p1menu" and p1_menu and not done_menu:
            keys(p1_menu)   # deja el cursor en Start Battle: la ✕ va en la vuelta siguiente
            done_menu = True
            last_press = time.time()
            continue
        press(ACTION[st])
        last_press = time.time()
        if st == "confirm":
            confirmed = True
    if shot and im is not None:
        im.save(shot)
    print(f"no llegó: {last_state} tras {time.time() - t0:.0f} s ({' '.join(hist[-8:])})")
    return 1


if __name__ == "__main__":
    sys.exit(main())
