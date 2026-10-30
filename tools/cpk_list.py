"""Lista los archivos de un CPK de CRI (el índice TOC, tablas @UTF sin cifrar).
uso: python tools/cpk_list.py <DATA.CPK> [filtro]   -> ruta  tamaño  tamaño_extraído  offset"""
import struct
import sys


def utf_table(buf):
    """Decodifica una tabla @UTF de CRI: devuelve (nombre, [filas como dict])."""
    assert buf[:4] == b"@UTF", buf[:4]
    size, rows_off, strings_off, data_off, name_off, ncols, row_len, nrows = struct.unpack(">IIIIIHHI", buf[4:32])
    base = 8
    strings = base + strings_off
    data = base + data_off

    def s(off):
        end = buf.index(b"\0", strings + off)
        return buf[strings + off:end].decode("utf-8", "replace")

    cols, p = [], 32
    for _ in range(ncols):
        flags = buf[p]; cname = s(struct.unpack(">I", buf[p + 1:p + 5])[0]); p += 5
        storage, typ = flags & 0xF0, flags & 0x0F
        const = None
        if storage == 0x30:   # valor constante guardado en la definición de la columna
            const, p = read(buf, p, typ, s, data)
        cols.append((cname, storage, typ, const))
    rows = []
    for r in range(nrows):
        p = base + rows_off + r * row_len
        row = {}
        for cname, storage, typ, const in cols:
            if storage == 0x50:
                row[cname], p = read(buf, p, typ, s, data)
            elif storage == 0x30:
                row[cname] = const
            else:
                row[cname] = None
        rows.append(row)
    return s(name_off), rows


def read(buf, p, typ, s, data):
    fmt = {0: ">B", 1: ">b", 2: ">H", 3: ">h", 4: ">I", 5: ">i", 6: ">Q", 7: ">q", 8: ">f"}.get(typ)
    if fmt:
        n = struct.calcsize(fmt)
        return struct.unpack(fmt, buf[p:p + n])[0], p + n
    if typ == 0xA:   # cadena
        return s(struct.unpack(">I", buf[p:p + 4])[0]), p + 4
    if typ == 0xB:   # datos: offset + tamaño
        off, n = struct.unpack(">II", buf[p:p + 8])
        return (off, n), p + 8
    raise ValueError("tipo UTF desconocido %x" % typ)


def main():
    path = sys.argv[1]
    filt = sys.argv[2].lower() if len(sys.argv) > 2 else ""
    with open(path, "rb") as f:
        hdr = f.read(0x800)
        assert hdr[:4] == b"CPK ", "no es un CPK"
        _, rows = utf_table(hdr[16:])
        h = rows[0]
        toc_off, content_off = h["TocOffset"], h.get("ContentOffset") or 0
        f.seek(toc_off)
        tag = f.read(16)
        assert tag[:4] == b"TOC ", tag[:4]
        size = struct.unpack("<I", tag[8:12])[0]
        _, files = utf_table(f.read(size))
    add = min(toc_off, content_off) if content_off else toc_off
    total = 0
    for r in files:
        name = (r.get("DirName") or "") + "/" + r["FileName"]
        if filt and filt not in name.lower():
            continue
        total += 1
        print("%-70s %10d %10d 0x%X" % (name, r["FileSize"], r["ExtractSize"], r["FileOffset"] + add))
    print("# %d archivos" % total, file=sys.stderr)


if __name__ == "__main__":
    main()
