"""Hace el parche (.hvpatch) de un mod de personaje: solo lo que el mod cambia, sin datos del disco. El port lo arma
con el DATA.CPK de cada jugador la primera vez que el juego lee ese archivo (hackversus/stubs.cpp, build_from_patch):
saca la entrada original, la descomprime, le aplica el parche y la vuelve a empaquetar con el mismo formato y tamaño.

El archivo editado tiene que medir lo mismo que el original descomprimido (extract_entry.py), y comprimido tiene que
caber en el tamaño de la entrada; si no cabe, el port lo dice en log.txt ("does not fit").

uso: python tools/mods/make_patch.py <DATA.CPK> <ruta en el CPK> <archivo editado> <salida.hvpatch> [posición color]
  posición: la del personaje en el carrusel (Sora 0, Sakuya 1, Haseo 2, Kite 3, BlackRose 4, Tsukasa 5, Ovan 6,
            Tokio 7); color: el índice del color nuevo (2 = el tercero). Con ellos imprime la línea de hv_mods.txt.

Formato (little-endian):
  "HVPATCH1", u32 largo de la entrada, u32 crc32 de la entrada original, u32 tamaño descomprimido original,
  u32 tamaño descomprimido nuevo, u32 crc32 del descomprimido nuevo, u32 ContentOffset, u32 Align, u32 trozos
  por trozo (en orden de ID): u32 offset en la entrada, u32 FileSize, u32 ExtractSize, u32 posición del campo FileSize
  u32 posición de ContentSize (u64 BE) o 0xFFFFFFFF, u32 posición de EnabledDataSize (u64 BE) o 0xFFFFFFFF
  u32 operaciones; cada una: u8 tipo (0 copiar del original, 1 insertar, 2 ceros), u32 offset en el original
  (0 si no copia), u32 largo; luego los bytes insertados, seguidos."""
import os
import re
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cpk import chunked_layout, read_entry, unpack_entry  # noqa: E402

cpk_path, name, newf, outp = sys.argv[1:5]
off, orig, _ = read_entry(cpk_path, name)
assert orig[:4] == b'CPK ', 'por ahora solo entradas "CPK interno" (como los modelos data/chr/*chrpsn.xfbin)'
h, table = chunked_layout(orig)
src = unpack_entry(orig)
dst = open(newf, 'rb').read()
assert len(dst) == len(src), 'el archivo editado mide %d bytes; tiene que medir %d, como el original' % (len(dst), len(src))

# diferencias: bloques de 32 bytes del original indexados, cada coincidencia se extiende hacia adelante y hacia atrás
B = 32
index = {}
for i in range(0, len(src) - B + 1, B):
    index.setdefault(src[i:i + B], i)
raw, t, pend = [], 0, 0          # pend: desde dónde aún no se copió (va como inserción)
while t + B <= len(dst):
    s = index.get(dst[t:t + B])
    if s is None:
        t += 1
        continue
    e = B
    while t + e < len(dst) and s + e < len(src) and dst[t + e] == src[s + e]:
        e += 1
    b = 0
    while t - b > pend and s - b > 0 and dst[t - b - 1] == src[s - b - 1]:
        b += 1
    if t - b > pend:
        raw.append((1, pend, t - b - pend))
    raw.append((0, s - b, e + b))
    t += e
    pend = t
if pend < len(dst):
    raw.append((1, pend, len(dst) - pend))

# las inserciones, con las tiras largas de ceros como operación propia (sin bytes)
ops, ins = [], bytearray()
for kind, a, ln in raw:
    if kind == 0:
        ops.append((0, a, ln))
        continue
    piece = dst[a:a + ln]
    i = 0
    for m in re.finditer(b'\0{256,}', piece):
        if m.start() > i:
            ops.append((1, 0, m.start() - i))
            ins += piece[i:m.start()]
        ops.append((2, 0, m.end() - m.start()))
        i = m.end()
    if i < ln:
        ops.append((1, 0, ln - i))
        ins += piece[i:]

# comprobar que reconstruye
re_ = bytearray()
k = 0
for kind, a, ln in ops:
    if kind == 1:
        re_ += ins[k:k + ln]
        k += ln
    elif kind == 2:
        re_ += bytes(ln)
    else:
        re_ += src[a:a + ln]
assert bytes(re_) == dst

out = bytearray(b'HVPATCH1')
out += struct.pack('<8I', len(orig), zlib.crc32(orig), len(src), len(dst), zlib.crc32(dst),
                   h['ContentOffset'][0], h['Align'][0], len(table))
for p in table:
    out += struct.pack('<4I', *p)
out += struct.pack('<2I', h['ContentSize'][1] if 'ContentSize' in h else 0xFFFFFFFF,
                   h['EnabledDataSize'][1] if 'EnabledDataSize' in h else 0xFFFFFFFF)
out += struct.pack('<I', len(ops))
for kind, a, ln in ops:
    out += struct.pack('<BII', kind, a, ln)
out += ins
open(outp, 'wb').write(out)
print('%s: %d bytes (%d operaciones, %d bytes nuevos de %d)' % (outp, len(out), len(ops), len(ins), len(dst)))
code = os.path.basename(name).split('chrpsn')[0]
if len(sys.argv) > 6:
    print('hv_mods.txt:\nsubst %s %s %s %X %d mods/%s' % (code, sys.argv[6], sys.argv[5], off, len(orig), os.path.basename(outp)))
else:
    print('hv_mods.txt: subst %s <color> <posición> %X %d mods/%s' % (code, off, len(orig), os.path.basename(outp)))
