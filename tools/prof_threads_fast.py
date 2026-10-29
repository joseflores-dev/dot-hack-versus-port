"""Resume un perfil HV_PROF por hilo, simbolizando todas las direcciones de libmain.so en UNA sola
invocación de llvm-symbolizer (prof_threads.py lanza un proceso por dirección: minutos por perfil).
uso: python tools/prof_threads_fast.py <log> <llvm-symbolizer> <libmain.so> [top_por_hilo=40]"""
import re
import subprocess
import sys
from collections import OrderedDict

log, sym, lib = sys.argv[1:4]
top = int(sys.argv[4]) if len(sys.argv) > 4 else 40
byline = len(sys.argv) > 5 and sys.argv[5] == 'lines'   # agrupar por función+línea
lines = open(log, encoding='utf8', errors='replace').read().splitlines()
host2g, gname = {}, {}
for l in lines:
    m = re.search(r'\[THREAD (\d+)\] host thread started.*hosttid=(\d+)', l)
    if m: host2g[m.group(2)] = 'g' + m.group(1)
    m = re.search(r'sys_ppu_thread_create tid=(\d+) name=("[^"]*")', l)
    if m: gname['g' + m.group(1)] = m.group(2)

threads = OrderedDict()   # tid -> [total, [(module, offset, samples)]]
cur = None
for l in lines:
    m = re.match(r'\[prof\] thread (\d+): (\d+)', l)
    if m:
        cur = m.group(1); threads[cur] = [int(m.group(2)), []]; continue
    m = re.match(r'\[prof\] (\S+) \+(0x[0-9a-f]+) (\d+)', l)
    if m and cur is not None:
        threads[cur][1].append((m.group(1), m.group(2), int(m.group(3))))

addrs = sorted({off for t in threads.values() for mod, off, _ in t[1] if 'libmain' in mod})
names = {}
if addrs:
    # por stdin: miles de direcciones no caben en la línea de órdenes de Windows
    p = subprocess.run([sym, '--obj=' + lib, '-f', '--no-inlines'], input='\n'.join(addrs) + '\n',
                       capture_output=True, text=True)
    if p.returncode != 0 or not p.stdout.strip():
        sys.stderr.write("symbolizer: rc=%d %s\n" % (p.returncode, p.stderr[:500]))
    blocks = p.stdout.replace('\r\n', '\n').split('\n\n')
    for a, b in zip(addrs, blocks):
        r = b.strip('\n').split('\n')
        names[a] = (r[0], r[1].split('/')[-1] if len(r) > 1 else '')

for tid, (total, rows) in threads.items():
    g = host2g.get(tid, '?')
    print(f"== host {tid} {g} {gname.get(g, '')}: {total} muestras")
    # agrupar por función
    byfn = OrderedDict()
    for mod, off, n in rows:
        if 'libmain' in mod:
            f, loc = names.get(off, (off, ''))
            if byline: f = f + ' ' + loc
        else:
            f, loc = mod.split('/')[-1], ''
        e = byfn.setdefault(f, [0, loc]); e[0] += n
    for f, (n, loc) in sorted(byfn.items(), key=lambda kv: -kv[1][0])[:top]:
        print(f"   {n:>6} {100.0 * n / total:5.1f}%  {f[:80]} {loc}")
