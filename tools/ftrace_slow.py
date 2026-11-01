"""Cuadros lentos de una traza PS3_FTRACE: qué cambia entre los cuadros lentos (> --slow ms) y los normales.

Para cada hilo PPU (por syscall/espera y sitio de llamada), cada SPU y cada fila del RSX, el promedio por cuadro en
los lentos y en los normales, ordenado por la diferencia. Además: dónde caen los cuadros lentos (el de antes y el de
después: ¿se compensan?) y el dibujo de los peores con ftrace_report.
uso: python tools/ftrace_slow.py frames/ft_<x>.txt [--slow 36] [--fast 34] [--draw 3]
"""
import argparse
import collections
import importlib.util
import os
import statistics

here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location(
    "ftr", os.path.join(here, "..", "ps3recomp-0.18.0", "tools", "ftrace_report.py"))
ftr = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ftr)


def frame_acc(spans, f0, f1):
    acc = collections.defaultdict(float)
    for k, who, a, b, t0, t1 in spans:
        if t1 < f0 or t0 > f1:
            continue
        o = ftr.overlap(t0, t1, f0, f1)
        if o <= 0:
            continue
        if k == ftr.USLEEP:
            acc[f"ppu {who} usleep @{b:#x}"] += o
        elif k == ftr.SYSCALL:
            acc[f"ppu {who} {ftr.SYSCALL_NAMES.get(a, f'sc{a}')} @{b:#x}"] += o
        elif k == ftr.SPU_BUSY:
            acc[f"spu {who} busy"] += o
            acc[f"spu wid {b:#x}"] += o
        elif k in (ftr.SEMA, ftr.SEMA_GAVEUP):
            acc["rsx acquire wait"] += o
        elif k == ftr.DRAIN:
            acc["rsx drain"] += o
        elif k == ftr.PRESENT:
            acc["rsx present"] += o
        elif k == ftr.GPU_ENCODE:
            acc["rsx encode " + ("present" if a else "flush")] += o
        elif k == ftr.GPU_WAIT:
            acc["rsx " + ftr.GPU_WAIT_KIND.get(a, "gpu wait")] += o
        elif k == ftr.READBACK:
            acc["rsx readback"] += o
        elif k == ftr.HLE:
            acc[f"ppu {who} hle {a:08X}"] += o
    return acc


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("trace")
    ap.add_argument("--slow", type=float, default=36.0)
    ap.add_argument("--fast", type=float, default=34.0)
    ap.add_argument("--draw", type=int, default=3)
    ap.add_argument("--ms-per-col", type=float, default=1.0)
    args = ap.parse_args()
    header, names, spans = ftr.load(args.trace)
    print(header)
    flips = [s[4] for s in spans if s[0] == ftr.FLIP_REQ]
    frames = list(zip(flips, flips[1:]))
    durs = [b - a for a, b in frames]
    slow = [i for i, d in enumerate(durs) if d > args.slow]
    fast = [i for i, d in enumerate(durs) if d <= args.fast]
    print(f"{len(frames)} cuadros, mediana {statistics.median(durs):.1f} ms; lentos (> {args.slow:g}) {len(slow)}, "
          f"normales (<= {args.fast:g}) {len(fast)}")
    print("duraciones:", " ".join(f"{d:.0f}" for d in durs))
    if not slow or not fast:
        return
    nb = [durs[i - 1] for i in slow if i > 0] + [durs[i + 1] for i in slow if i + 1 < len(durs)]
    print(f"vecinos de los lentos: promedio {statistics.mean(nb):.1f} ms (¿compensan?)")
    accs = [frame_acc(spans, a, b) for a, b in frames]
    keys = set().union(*accs)
    rows = []
    for k in keys:
        ms = statistics.mean(accs[i][k] for i in slow)
        mf = statistics.mean(accs[i][k] for i in fast)
        rows.append((ms - mf, ms, mf, k))
    print(f"\n{'diferencia':>10} {'lentos':>7} {'normales':>8}  (ms por cuadro)")
    for d, ms, mf, k in sorted(rows, key=lambda r: -abs(r[0]))[:30]:
        t = k.split()
        label = k
        if t[0] == "ppu":
            label = f"ppu {t[1]} {names.get(int(t[1]), '?')[:18]:18s} " + " ".join(t[2:])
        print(f"{d:+10.1f} {ms:7.1f} {mf:8.1f}  {label}")
    # dibujo de los peores cuadros: reusar el pintor de ftrace_report con esos cuadros
    worst = sorted(slow, key=lambda i: -durs[i])[:args.draw]
    for i in worst:
        f0, f1 = frames[i]
        print(f"\ncuadro {i}: {durs[i]:.1f} ms (antes {durs[i - 1] if i else 0:.1f}, "
              f"después {durs[i + 1] if i + 1 < len(durs) else 0:.1f})")
        a = accs[i]
        for k, v in sorted(a.items(), key=lambda kv: -kv[1])[:14]:
            print(f"    {v:6.1f}  {k}")


if __name__ == "__main__":
    main()
