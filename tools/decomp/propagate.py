"""Propaga las familias de map_modules.py por el grafo de llamadas (bl directos): una función sin etiqueta toma
la familia mayoritaria de sus vecinas (quién la llama y a quién llama), en varias pasadas; las funciones hoja
de libc (memcpy, strlen...) no arrastran a nadie: se ignoran las vecinas muy llamadas.

uso: python tools/decomp/propagate.py [game/EBOOT.ELF] [analysis/functions.json] [analysis/module_map.json]
salida: analysis/module_map_full.json  ({función: familia}) y resumen por familia
"""
import bisect, collections, json, struct, sys

elf = sys.argv[1] if len(sys.argv) > 1 else 'game/EBOOT.ELF'
fjs = sys.argv[2] if len(sys.argv) > 2 else 'analysis/functions.json'
mm = sys.argv[3] if len(sys.argv) > 3 else 'analysis/module_map.json'

d = open(elf, 'rb').read()
phoff, = struct.unpack_from('>Q', d, 0x20)
phentsize, phnum = struct.unpack_from('>HH', d, 0x36)
text = []
for i in range(phnum):
    p = phoff + i * phentsize
    ptype, flags, off, vaddr, paddr, filesz, memsz = struct.unpack_from('>IIQQQQQ', d, p)
    if ptype == 1 and flags & 1:
        text.append((vaddr, off, filesz))
funcs = sorted((int(f['start'], 16), int(f['end'], 16)) for f in json.load(open(fjs)))
starts = [s for s, _ in funcs]
size = dict(funcs)


def owner(a):
    i = bisect.bisect_right(starts, a) - 1
    return funcs[i][0] if i >= 0 and a < funcs[i][1] else None


callers = collections.defaultdict(set)
callees = collections.defaultdict(set)
for v, o, sz in text:
    for k in range(0, sz, 4):
        ins, = struct.unpack_from('>I', d, o + k)
        if ins >> 26 == 18 and ins & 1 and not ins & 2:        # bl (relativo, con enlace)
            li = ins & 0x03FFFFFC
            li = li - 0x04000000 if li & 0x02000000 else li
            t = owner(v + k + li)
            f = owner(v + k)
            if t is not None and f is not None and t != f:
                callers[t].add(f)
                callees[f].add(t)

label = {int(k, 16): v for k, v in json.load(open(mm))['labels'].items()}
seed = dict(label)
hub = {f for f in starts if len(callers[f]) > 60}                # utilidades muy compartidas: no votan
for it in range(12):
    new = {}
    for f in starts:
        if f in label:
            continue
        votes = collections.Counter()
        for g in callers[f] | callees[f]:
            if g in label and g not in hub:
                votes[label[g]] += 1
        if votes:
            top, n = votes.most_common(1)[0]
            if n >= 1 and n >= sum(votes.values()) * 0.6:
                new[f] = top
    if not new:
        break
    label.update(new)

tot = sum(e - s for s, e in funcs)
fam_f = collections.Counter(label.values())
fam_b = collections.Counter()
for f, l in label.items():
    fam_b[l] += size[f] - f
json.dump({'%08X' % k: v for k, v in label.items()}, open('analysis/module_map_full.json', 'w'), indent=0)
print('llamadas bl: %d aristas; utilidades compartidas excluidas del voto: %d' % (
    sum(len(v) for v in callees.values()), len(hub)))
print('etiquetadas: %d de %d funciones (%.0f%% del código)' % (
    len(label), len(funcs), 100 * sum(fam_b.values()) / tot))
for k, v in fam_b.most_common():
    print('  %-5s %6d funciones  %6d KB' % (k, fam_f[k], v // 1024))
print('sin etiqueta: %d funciones, %d KB' % (len(funcs) - len(label), (tot - sum(fam_b.values())) // 1024))
