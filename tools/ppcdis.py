"""Desensambla N instrucciones PPC64 big-endian de un ELF de PS3 en una dirección virtual.

uso: python ppcdis.py <elf> <addr_hex> [n]
"""
import struct
import sys

import capstone

d = open(sys.argv[1], 'rb').read()
addr = int(sys.argv[2], 16)
n = int(sys.argv[3]) if len(sys.argv) > 3 else 16
phoff = struct.unpack('>Q', d[32:40])[0]
for i in range(struct.unpack('>H', d[56:58])[0]):
    o = phoff + i * 56
    t, fl, off, va, pa, fs, ms, al = struct.unpack('>IIQQQQQQ', d[o:o + 56])
    if t == 1 and va <= addr < va + fs:
        code = d[off + addr - va: off + addr - va + 4 * n]
        break
md = capstone.Cs(capstone.CS_ARCH_PPC, capstone.CS_MODE_64 | capstone.CS_MODE_BIG_ENDIAN)
for ins in md.disasm(code, addr):
    print('%08x  %-8s %s' % (ins.address, ins.mnemonic, ins.op_str))
