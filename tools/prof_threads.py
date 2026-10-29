"""Resume un perfil HV_PROF por hilo (log de la app con [prof] y las líneas hosttid / sys_ppu_thread_create).
uso: python tools/prof_threads.py <log> <llvm-symbolizer> <libmain.so>"""
import re
import subprocess
import sys

log, sym, lib = sys.argv[1:4]
lines = open(log, encoding='utf8', errors='replace').read().splitlines()
host2g, gname = {}, {}
for l in lines:
    m = re.search(r'\[THREAD (\d+)\] host thread started.*hosttid=(\d+)', l)
    if m: host2g[m.group(2)] = 'g' + m.group(1)
    m = re.search(r'sys_ppu_thread_create tid=(\d+) name=("[^"]*")', l)
    if m: gname['g' + m.group(1)] = m.group(2)
for l in lines:
    m = re.match(r'\[prof\] thread (\d+): (\d+)', l)
    if m:
        g = host2g.get(m.group(1), '?')
        print(f"== host {m.group(1)} {g} {gname.get(g, '')}: {m.group(2)} muestras")
        continue
    m = re.match(r'\[prof\] (\S+) \+(0x[0-9a-f]+) (\d+)', l)
    if m:
        if 'libmain' in m.group(1):
            r = subprocess.run([sym, '--obj=' + lib, '-f', m.group(2)], capture_output=True, text=True).stdout.split('\n')
            f, loc = r[0], (r[1].split('/')[-1] if len(r) > 1 else '')
        else:
            f, loc = m.group(1).split('/')[-1] + '+' + m.group(2), ''
        print(f"   {m.group(3):>5} {f[:70]} {loc}")
