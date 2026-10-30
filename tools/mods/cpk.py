"""Lectura de los CPK de CRI de .hack//Versus para las herramientas de mods: tablas @UTF, el índice (TOC), CRILAYLA y
las entradas "CPK interno" (un CPK dentro del CPK, con el archivo partido en trozos de 128 KB comprimidos)."""
import struct


def utf_cols(buf, base_abs):
    """Tabla @UTF -> (columnas, [filas: {columna: (valor, posición absoluta, formato)}])."""
    assert buf[:4] == b'@UTF', buf[:4]
    size, rows_off, strings_off, data_off, name_off, ncols, row_len, nrows = struct.unpack('>IIIIIHHI', buf[4:32])
    strings = 8 + strings_off

    def s(off):
        end = buf.index(b'\0', strings + off)
        return buf[strings + off:end].decode('utf-8', 'replace')

    fmts = {0: '>B', 1: '>b', 2: '>H', 3: '>h', 4: '>I', 5: '>i', 6: '>Q', 7: '>q', 8: '>f'}
    cols, p = [], 32
    for _ in range(ncols):
        flags = buf[p]
        name = s(struct.unpack('>I', buf[p + 1:p + 5])[0])
        p += 5
        storage, typ = flags & 0xF0, flags & 0x0F
        if storage == 0x30:   # constante guardada en la definición de la columna
            p += struct.calcsize(fmts[typ]) if typ in fmts else (4 if typ == 0xA else 8)
        cols.append((name, storage, typ))
    rows = []
    for r in range(nrows):
        p = 8 + rows_off + r * row_len
        row = {}
        for name, storage, typ in cols:
            if storage != 0x50:
                continue
            if typ in fmts:
                n = struct.calcsize(fmts[typ])
                row[name] = (struct.unpack(fmts[typ], buf[p:p + n])[0], base_abs + p, fmts[typ])
                p += n
            elif typ == 0xA:
                row[name] = (s(struct.unpack('>I', buf[p:p + 4])[0]), base_abs + p, None)
                p += 4
            elif typ == 0xB:
                row[name] = (struct.unpack('>II', buf[p:p + 8]), base_abs + p, None)
                p += 8
        rows.append(row)
    return cols, rows


def find_entry(cpk_path, name):
    """(offset absoluto, tamaño en el CPK, tamaño extraído) de un archivo del CPK por su ruta (data/chr/...)."""
    with open(cpk_path, 'rb') as f:
        hdr = f.read(0x800)
        assert hdr[:4] == b'CPK ', 'no es un CPK'
        _, hrows = utf_cols(hdr[16:], 16)
        h = hrows[0]
        toc = h['TocOffset'][0]
        content = h['ContentOffset'][0] if 'ContentOffset' in h else 0
        f.seek(toc)
        tag = f.read(16)
        assert tag[:4] == b'TOC ', tag[:4]
        _, rows = utf_cols(f.read(struct.unpack('<I', tag[8:12])[0]), toc + 16)
    add = min(toc, content) if content else toc
    for r in rows:
        full = (r['DirName'][0] if 'DirName' in r else '') + '/' + r['FileName'][0]
        if full.lower() == name.lower().lstrip('/') or full.lower().endswith('/' + name.lower().lstrip('/')):
            return r['FileOffset'][0] + add, r['FileSize'][0], r['ExtractSize'][0]
    raise SystemExit('no está en el CPK: %s' % name)


def crilayla(src):
    """Descomprime CRILAYLA: los primeros 0x100 bytes van tal cual al final; el resto se lee de atrás hacia adelante."""
    usize, hoff = struct.unpack_from('<II', src, 8)
    out = bytearray(usize + 0x100)
    out[:0x100] = src[16 + hoff:16 + hoff + 0x100]
    ptr = 16 + hoff - 1
    pool = left = 0

    def bits(n):
        nonlocal ptr, pool, left
        v = 0
        while n:
            if not left:
                pool = src[ptr]
                ptr -= 1
                left = 8
            t = left if left < n else n
            v = (v << t) | ((pool >> (left - t)) & ((1 << t) - 1))
            left -= t
            n -= t
        return v

    end = 0x100 + usize - 1
    done = 0
    vle = (2, 3, 5, 8)
    while done < usize:
        if bits(1):
            ref = end - done + bits(13) + 3
            n, lvl = 3, 0
            while True:
                x = bits(vle[lvl])
                n += x
                if x != (1 << vle[lvl]) - 1:
                    break
                if lvl < 3:
                    lvl += 1
            for _ in range(n):
                if done >= usize:
                    break
                out[end - done] = out[ref]
                ref -= 1
                done += 1
        else:
            out[end - done] = bits(8)
            done += 1
    return bytes(out)


def chunked_layout(entry):
    """De una entrada "CPK interno": (cabecera, trozos en orden de ID como (offset, FileSize, ExtractSize,
    posición del campo FileSize)), con la cabecera como dict de utf_cols."""
    assert entry[:4] == b'CPK ', 'la entrada no es un CPK interno'
    _, hrows = utf_cols(entry[0x10:], 0x10)
    h = hrows[0]
    content, align, itoc = h['ContentOffset'][0], h['Align'][0], h['ItocOffset'][0]
    tbl = entry[itoc + 16:]
    base_data = itoc + 16 + 8 + struct.unpack('>I', tbl[16:20])[0]
    _, rows = utf_cols(tbl, itoc + 16)
    o, n = rows[0]['DataH'][0]
    _, chunks = utf_cols(entry[base_data + o: base_data + o + n], base_data + o)
    chunks.sort(key=lambda r: r['ID'][0])
    pos, table = content, []
    for r in chunks:   # seguidos desde ContentOffset, alineados, en orden de ID
        pos += (-pos) % align
        table.append((pos, r['FileSize'][0], r['ExtractSize'][0], r['FileSize'][1]))
        pos += r['FileSize'][0]
    return h, table


def unpack_entry(entry):
    """El archivo de una entrada, sin comprimir (CRILAYLA, CPK interno o tal cual)."""
    if entry[:4] == b'CPK ':
        _, table = chunked_layout(entry)
        parts = []
        for o, fs, es, _ in table:
            blob = entry[o:o + fs]
            parts.append(crilayla(blob) if fs < es else blob)
            assert len(parts[-1]) == es, 'trozo de %d bytes, se esperaban %d' % (len(parts[-1]), es)
        return b''.join(parts)
    if entry[:8] == b'CRILAYLA':
        return crilayla(entry)
    return entry


def read_entry(cpk_path, name):
    off, size, ext = find_entry(cpk_path, name)
    with open(cpk_path, 'rb') as f:
        f.seek(off)
        return off, f.read(size), ext
