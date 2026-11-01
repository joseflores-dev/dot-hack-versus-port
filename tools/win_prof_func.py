"""Perfil HV_PROF del PC (out-win/run.err) por hilo: las funciones más caras de los hilos más ocupados, con la
tabla de símbolos del exe (llvm-nm). Opcional: porcentaje de una función en cada hilo.
uso: python tools/win_prof_func.py out-win/nm.txt out-win/run.err [n_hilos=4] [top=12] [función]"""
import bisect, collections, re, sys

nm, log = sys.argv[1:3]
nth = int(sys.argv[3]) if len(sys.argv) > 3 else 4
top = int(sys.argv[4]) if len(sys.argv) > 4 else 12
want = sys.argv[5] if len(sys.argv) > 5 else None
syms = []
for line in open(nm, encoding='utf-8', errors='replace'):
    p = line.split()
    if len(p) >= 3 and p[1].lower() in 'tw':
        syms.append((int(p[0], 16) - 0x140000000, p[2]))
syms.sort()
addrs = [a for a, _ in syms]
by = collections.defaultdict(collections.Counter)
for line in open(log, encoding='utf-8', errors='replace'):
    m = re.match(r'\[prof\] (\d+) \S* ?(exe\+0x([0-9a-f]+)|abs 0x[0-9a-f]+) (\d+)$', line.strip())
    if not m:
        continue
    name = '(fuera del exe)'
    if m.group(3):
        i = bisect.bisect_right(addrs, int(m.group(3), 16)) - 1
        name = syms[i][1] if i >= 0 else '?'
    by[m.group(1)][name] += int(m.group(4))
busy = sorted(by.items(), key=lambda kv: -(sum(kv[1].values()) - kv[1]['(fuera del exe)']))
for tid, c in busy[:nth]:
    own = sum(c.values()) - c['(fuera del exe)']
    print('== hilo %s: %d muestras en el exe' % (tid, own))
    if want:
        print('   %s: %.1f%%' % (want, 100 * c[want] / max(own, 1)))
    for k, v in c.most_common(top + 1):
        if k != '(fuera del exe)':
            print('   %6d %5.1f%%  %s' % (v, 100 * v / max(own, 1), k[:90]))
