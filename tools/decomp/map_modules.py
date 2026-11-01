"""Mapa del código del EBOOT por familias (paso 1 del decomp): cada función se etiqueta por las cadenas que carga
(rutas de archivo fuente, mensajes de Lua, CRI, sonido nus, SPURS...) y el resultado se resume por bloques de
direcciones, donde el enlazador dejó juntos los objetos de cada biblioteca.

uso: python tools/decomp/map_modules.py [game/EBOOT.ELF] [analysis/functions.json] [salida.json]
"""
import bisect, collections, json, re, struct, sys

elf = sys.argv[1] if len(sys.argv) > 1 else 'game/EBOOT.ELF'
fjs = sys.argv[2] if len(sys.argv) > 2 else 'analysis/functions.json'
out = sys.argv[3] if len(sys.argv) > 3 else 'analysis/module_map.json'
TOC = 0x98FE58
BLOCK = 0x10000

d = open(elf, 'rb').read()
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
    b = rd(addr, 200)
    if not b:
        return None
    s = b.split(b'\0')[0]
    return s.decode('latin1') if len(s) >= 4 and all(32 <= c < 127 or c in (9, 10) for c in s) else None


# familias por palabras clave (orden = prioridad)
FAM = [
    ('lua', re.compile(r"\blua_|luaL_|LUA_|'%s' expected|attempt to (call|index|perform)|stack overflow|"
                       r"\b_ENV\b|metatable|coroutine|__index|__newindex|string\.format|bad argument")),
    ('cri', re.compile(r'\bCRI|cri[A-Z_]|criware|ADX|Sofdec|\bSFD|\bAHX|CriFs|E\d{10}', re.I)),
    ('nus', re.compile(r'\bnus|bnus|\.nub\b|NUB', re.I)),
    ('nucc', re.compile(r'nucc|Nucc|NU::|nucclib|xfbin|XFBIN|NUCC')),
    ('game', re.compile(r'hackVS_ROM/prog|\bcc[A-Z]\w+|cmn[A-Z]\w+')),
    ('sony', re.compile(r'\bcell[A-Z]\w+|sys_|SPURS|Spurs|libgcm|\bgcm|sce[A-Z]|PSN|NP[A-Z]')),
    ('libc', re.compile(r'%[0-9.]*[dsfxXp]|\bmalloc|\bfree|assert|abort|errno')),
]

funcs = sorted((int(f['start'], 16), int(f['end'], 16)) for f in json.load(open(fjs)))
starts = [s for s, _ in funcs]


def owner(a):
    i = bisect.bisect_right(starts, a) - 1
    return funcs[i][0] if i >= 0 and a < funcs[i][1] else None


fstr = collections.defaultdict(list)
for v, o, sz, fl in segs:
    if not fl & 1:
        continue
    for k in range(0, sz - 4, 4):
        ins, = struct.unpack_from('>I', d, o + k)
        op = ins >> 26
        if op not in (32, 58) or ((ins >> 16) & 31) != 2:
            continue
        disp = ins & 0xFFFF
        disp = disp - 0x10000 if disp & 0x8000 else disp
        val = rd(TOC + (disp & ~3 if op == 58 else disp), 4)
        if not val:
            continue
        base, = struct.unpack('>I', val)
        rt = (ins >> 21) & 31
        cands = [base]
        for j in range(1, 7):
            if k + 4 * j + 4 > sz:
                break
            nx, = struct.unpack_from('>I', d, o + k + 4 * j)
            if nx >> 26 == 14 and ((nx >> 16) & 31) == rt:
                imm = nx & 0xFFFF
                cands.append(base + (imm - 0x10000 if imm & 0x8000 else imm))
        f = owner(v + k)
        if f is None:
            continue
        for a in cands:
            s = cstr(a)
            if s:
                fstr[f].append(s)

label = {}
for f, ss in fstr.items():
    votes = collections.Counter()
    for s in ss:
        for name, rx in FAM:
            if rx.search(s):
                votes[name] += 1
                break
    if votes:
        label[f] = votes.most_common(1)[0][0]

# resumen por bloques: familia dominante (por funciones etiquetadas) y tamaño de código
blocks = collections.defaultdict(collections.Counter)
bsize = collections.Counter()
for s, e in funcs:
    b = s // BLOCK
    bsize[b] += e - s
    if s in label:
        blocks[b][label[s]] += 1
rows = []
for b in sorted(bsize):
    c = blocks[b]
    dom = c.most_common(1)[0][0] if c else '?'
    rows.append((b * BLOCK, dom, sum(c.values()), dict(c), bsize[b]))
# tramos contiguos con la misma familia dominante
spans = []
for a, dom, n, c, sz in rows:
    if spans and spans[-1][2] == dom:
        spans[-1][1] = a + BLOCK
        spans[-1][3] += sz
    else:
        spans.append([a, a + BLOCK, dom, sz])
json.dump({'labels': {'%08X' % k: v for k, v in label.items()},
           'strings': {'%08X' % k: v[:8] for k, v in fstr.items()},
           'spans': spans}, open(out, 'w'), indent=0)
tot = sum(e - s for s, e in funcs)
print('funciones con cadenas: %d, etiquetadas: %d de %d' % (len(fstr), len(label), len(funcs)))
print('por familia (funciones):', dict(collections.Counter(label.values())))
fam_bytes = collections.Counter()
for a, b, dom, sz in spans:
    fam_bytes[dom] += sz
print('código por familia dominante del tramo (KB):', {k: v // 1024 for k, v in fam_bytes.most_common()})
for a, b, dom, sz in spans:
    print('%08X-%08X  %-5s %6d KB' % (a, b, dom, sz // 1024))
