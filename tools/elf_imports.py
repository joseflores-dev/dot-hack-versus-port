"""Lista secciones y librerías importadas (sceLibStub) de un ELF PPU de PS3."""
import struct
import sys

d = open(sys.argv[1], 'rb').read()
shoff = struct.unpack('>Q', d[40:48])[0]
shnum, shstrndx = struct.unpack('>HH', d[60:64])
secs = []
for i in range(shnum):
    o = shoff + i * 64
    name, typ, flags, addr, off, size = struct.unpack('>IIQQQQ', d[o:o + 40])
    secs.append([name, typ, addr, off, size])
stro = secs[shstrndx][3]
for s in secs:
    s[0] = d[stro + s[0]:d.index(b'\0', stro + s[0])].decode()
    print('%-22s addr=%08x size=%8x' % (s[0], s[2], s[4]))

# segmentos para traducir direcciones virtuales a offsets
phoff = struct.unpack('>Q', d[32:40])[0]
phnum = struct.unpack('>H', d[56:58])[0]
segs = []
for i in range(phnum):
    o = phoff + i * 56
    t, fl, off, va, pa, fs, ms, al = struct.unpack('>IIQQQQQQ', d[o:o + 56])
    if t == 1 and fs:
        segs.append((va, off, fs))


def v2o(va):
    for sva, off, fs in segs:
        if sva <= va < sva + fs:
            return off + va - sva
    return None


def cstr(va):
    o = v2o(va)
    return d[o:d.index(b'\0', o)].decode('ascii', 'replace') if o is not None else '?'


# sys_process_prx_param (segmento 0x60000002): libstub_start/end
prx = None
for i in range(phnum):
    o = phoff + i * 56
    t, fl, off, va = struct.unpack('>IIQQ', d[o:o + 24])
    if t == 0x60000002:
        prx = off
size, magic, ver, _, ent_s, ent_e, stub_s, stub_e = struct.unpack('>IIIIIIII', d[prx:prx + 32])
assert magic == 0x1b434cec, hex(magic)
total = 0
print('\nlibrerías importadas:')
for off in range(v2o(stub_s), v2o(stub_e), 0x2c):
    hdr = d[off:off + 0x2c]
    size, _, ver, attr, nfunc, nvar, ntls, _, _, name_va, fnid_va, fstub_va = struct.unpack('>BBHHHHHBB2xIII', hdr[:0x1c])
    total += nfunc
    print('  %-24s funcs=%3d vars=%d' % (cstr(name_va), nfunc, nvar))
print('total funciones importadas:', total)
