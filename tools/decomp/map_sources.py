"""Mapa del código del EBOOT (paso 1 del decomp): atribuye funciones a archivos fuente por las cadenas que cargan.

Las funciones de .hack//Versus que validan algo llevan la ruta de su archivo fuente en el mensaje
(c:/usr/hackVS_ROM/prog/source/ccPlayerParam.cpp ...). El código carga esas direcciones desde la TOC
(lwz rX, d(r2)), o desde una entrada de la TOC más un desplazamiento (addi). Cada función que carga una de
esas cadenas queda asignada a su archivo; los vecinos sin cadena se asignan después por cercanía (el enlazador
deja juntas las funciones de un mismo objeto).

uso: python tools/decomp/map_sources.py [game/EBOOT.ELF] [analysis/functions.json] [salida.json]
"""
import bisect, json, re, struct, sys

elf = sys.argv[1] if len(sys.argv) > 1 else 'game/EBOOT.ELF'
fjs = sys.argv[2] if len(sys.argv) > 2 else 'analysis/functions.json'
out = sys.argv[3] if len(sys.argv) > 3 else 'analysis/source_map.json'
TOC = 0x98FE58

d = open(elf, 'rb').read()
# segmentos PT_LOAD (ELF64 big-endian)
phoff, = struct.unpack_from('>Q', d, 0x20)
phentsize, phnum = struct.unpack_from('>HH', d, 0x36)
segs = []
for i in range(phnum):
    p = phoff + i * phentsize
    ptype, flags, off, vaddr, paddr, filesz, memsz = struct.unpack_from('>IIQQQQQ', d, p)
    if ptype == 1:
        segs.append((vaddr, off, filesz, flags))


def rd(addr, n):
    for v, o, sz, _ in segs:
        if v <= addr < v + sz:
            return d[o + addr - v: o + addr - v + n]
    return None


def cstr(addr):
    b = rd(addr, 256)
    if not b:
        return None
    s = b.split(b'\0')[0]
    return s.decode('latin1') if len(s) >= 4 and all(32 <= c < 127 for c in s) else None


funcs = sorted((int(f['start'], 16), int(f['end'], 16)) for f in json.load(open(fjs)))
starts = [s for s, _ in funcs]


def owner(a):
    i = bisect.bisect_right(starts, a) - 1
    return funcs[i][0] if i >= 0 and a < funcs[i][1] else None


SRC = re.compile(r'([A-Za-z]:/[^\s"]+?\.(?:cpp|c|cc|h|hpp))')
fsrc = {}          # función -> {archivo: veces}
text = [s for s in segs if s[3] & 1]
for v, o, sz, _ in text:
    for k in range(0, sz - 4, 4):
        ins, = struct.unpack_from('>I', d, o + k)
        op = ins >> 26
        if op not in (32, 58) or ((ins >> 16) & 31) != 2:      # lwz / ld con base r2
            continue
        disp = ins & 0xFFFF
        disp = disp - 0x10000 if disp & 0x8000 else disp
        if op == 58:
            disp &= ~3
        slot = TOC + disp
        val = rd(slot, 4)
        if not val:
            continue
        base, = struct.unpack('>I', val)
        rt = (ins >> 21) & 31
        cands = [base]
        # addi rY, rt, imm en las 6 instrucciones siguientes: cadena = base + imm
        for j in range(1, 7):
            if o + k + 4 * j + 4 > o + sz:
                break
            nx, = struct.unpack_from('>I', d, o + k + 4 * j)
            if nx >> 26 == 14 and ((nx >> 16) & 31) == rt:
                imm = nx & 0xFFFF
                cands.append(base + (imm - 0x10000 if imm & 0x8000 else imm))
        for a in cands:
            s = cstr(a)
            if s:
                m = SRC.search(s)
                if m:
                    f = owner(v + k)
                    if f is not None:
                        fsrc.setdefault(f, {}).setdefault(m.group(1), 0)
                        fsrc[f][m.group(1)] += 1
                    break

# por cercanía: una función sin cadena hereda el archivo de sus vecinas si coinciden
assign = {f: max(c, key=c.get) for f, c in fsrc.items()}
direct = dict(assign)
order = [s for s, _ in funcs]
idx = {s: i for i, s in enumerate(order)}
known = sorted(idx[f] for f in assign)
for a, b in zip(known, known[1:]):
    fa, fb = assign[order[a]], assign[order[b]]
    if fa == fb and b - a < 400:
        for i in range(a + 1, b):
            assign.setdefault(order[i], fa)

json.dump({'direct': {'%08X' % k: v for k, v in direct.items()},
           'assigned': {'%08X' % k: v for k, v in assign.items()}}, open(out, 'w'), indent=0)
by = {}
for f, s in assign.items():
    by.setdefault(s, []).append(f)
tot_bytes = sum(e - s for s, e in funcs)
fsize = dict(funcs)
print('funciones con cadena de archivo fuente: %d; asignadas con vecinas: %d de %d' % (len(direct), len(assign), len(funcs)))
asg_bytes = sum(fsize[f] - f for f in assign)
print('código asignado: %.1f%% de %d bytes' % (100 * asg_bytes / tot_bytes, tot_bytes))
for s, fl in sorted(by.items(), key=lambda kv: -len(kv[1]))[:40]:
    print('%5d  %08X-%08X  %s' % (len(fl), min(fl), max(fl), s.split('/source/')[-1]))
