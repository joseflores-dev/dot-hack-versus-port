"""Saca un archivo del DATA.CPK de .hack//Versus ya descomprimido, para editarlo y hacer un mod con make_patch.py.
uso: python tools/mods/extract_entry.py <DATA.CPK> <ruta en el CPK, p. ej. data/chr/1kit01chrpsn.xfbin> <salida>
(las rutas: python tools/cpk_list.py <DATA.CPK> chrpsn)"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cpk import read_entry, unpack_entry  # noqa: E402

cpk_path, name, dest = sys.argv[1:4]
off, entry, ext = read_entry(cpk_path, name)
data = unpack_entry(entry)
assert len(data) == ext, 'extraído %d bytes, el índice dice %d' % (len(data), ext)
open(dest, 'wb').write(data)
print('%s: %d bytes (en el CPK: 0x%X, %d bytes, %s)' % (dest, len(data), off, len(entry),
                                                     'CPK interno' if entry[:4] == b'CPK ' else entry[:8]))
