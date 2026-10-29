"""Lista y extrae archivos de una ISO9660 (ISO de PS3) sin dependencias.

uso: python iso_extract.py <iso> list
     python iso_extract.py <iso> get <ruta/en/iso> <destino>
     python iso_extract.py <iso> all <carpeta>
"""
import os
import struct
import sys

SECTOR = 2048


def read_dir(f, lba, size):
    out = []
    f.seek(lba * SECTOR)
    data = f.read(size)
    off = 0
    while off < len(data):
        ln = data[off]
        if ln == 0:  # resto del sector vacío
            off = (off // SECTOR + 1) * SECTOR
            continue
        rec = data[off:off + ln]
        ext_lba = struct.unpack_from('<I', rec, 2)[0]
        ext_len = struct.unpack_from('<I', rec, 10)[0]
        flags = rec[25]
        name_len = rec[32]
        name = rec[33:33 + name_len]
        off += ln
        if name in (b'\x00', b'\x01'):
            continue
        name = name.decode('ascii', 'replace').split(';')[0]
        # archivos >4 GB vienen en varios extents con el mismo nombre (flag 0x80)
        if out and out[-1][0] == name and out[-1][4]:
            out[-1][2].append((ext_lba, ext_len))
            out[-1][4] = bool(flags & 0x80)
            continue
        out.append([name, bool(flags & 2), [(ext_lba, ext_len)], 0, bool(flags & 0x80)])
    return out


def walk(f, lba, size, prefix=''):
    for name, is_dir, extents, _, _ in read_dir(f, lba, size):
        path = prefix + name
        if is_dir:
            yield path + '/', True, extents
            yield from walk(f, extents[0][0], extents[0][1], path + '/')
        else:
            yield path, False, extents


def root(f):
    f.seek(16 * SECTOR)
    pvd = f.read(SECTOR)
    assert pvd[1:6] == b'CD001', 'no es ISO9660'
    rec = pvd[156:156 + 34]
    return struct.unpack_from('<I', rec, 2)[0], struct.unpack_from('<I', rec, 10)[0]


def main():
    iso, cmd = sys.argv[1], sys.argv[2]
    with open(iso, 'rb') as f:
        lba, size = root(f)
        if cmd == 'list':
            for path, is_dir, extents in walk(f, lba, size):
                if not is_dir:
                    print('%12d  %s' % (sum(l for _, l in extents), path))
        elif cmd == 'all':
            for path, is_dir, extents in walk(f, lba, size):
                dest = os.path.join(sys.argv[3], path)
                os.makedirs(os.path.dirname(dest), exist_ok=True)
                if is_dir:
                    os.makedirs(dest, exist_ok=True)
                    continue
                with open(dest, 'wb') as o:
                    for elba, elen in extents:
                        f.seek(elba * SECTOR)
                        while elen:
                            chunk = f.read(min(elen, 1 << 24))
                            o.write(chunk)
                            elen -= len(chunk)
            print('extraído en', sys.argv[3])
        elif cmd == 'get':
            want, dest = sys.argv[3].upper(), sys.argv[4]
            for path, is_dir, extents in walk(f, lba, size):
                if path.upper() == want and not is_dir:
                    os.makedirs(os.path.dirname(dest) or '.', exist_ok=True)
                    with open(dest, 'wb') as o:
                        for elba, elen in extents:
                            f.seek(elba * SECTOR)
                            o.write(f.read(elen))
                    print('extraído', path, '->', dest)
                    return
            sys.exit('no encontrado: ' + want)


if __name__ == '__main__':
    main()
