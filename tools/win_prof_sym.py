# Agrupa las muestras de [prof] (stubs.cpp, Windows) por función usando la tabla de símbolos del .exe.
# uso: python tools/win_prof_sym.py out-win/nm.txt out-win/run.err <tid> [n]
import bisect, collections, re, sys
syms = []
for line in open(sys.argv[1], encoding='utf-8', errors='replace'):
    p = line.split()
    if len(p) >= 3 and p[1].lower() in 'tw': syms.append((int(p[0], 16) - 0x140000000, p[2]))
syms.sort(); addrs = [a for a, _ in syms]
tid = sys.argv[3]; n = int(sys.argv[4]) if len(sys.argv) > 4 else 20
agg = collections.Counter(); tot = 0
for line in open(sys.argv[2], encoding='utf-8', errors='replace'):
    m = re.match(r'\[prof\] (\d+) \S* ?(exe\+0x([0-9a-f]+)|abs 0x[0-9a-f]+) (\d+)$', line.strip())
    if not m or m.group(1) != tid: continue
    c = int(m.group(4)); tot += c
    if m.group(3):
        i = bisect.bisect_right(addrs, int(m.group(3), 16)) - 1
        agg[syms[i][1] if i >= 0 else '?'] += c
    else: agg['(fuera del exe: espera/sistema)'] += c
for name, c in agg.most_common(n): print(f'{c:6d} {name}')
print('total (solo top 25 direcciones):', tot)
