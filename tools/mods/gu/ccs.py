"""Lector de archivos CCS de .hack//G.U. (PS2, CCSF 0x123; Last Recode 0x124/0x131): huesos, mallas, texturas.

Formato tomado de StudioCCS (NCDyson, libCCS) como referencia y comprobado contra los archivos de Redemption.
Las secciones se leen en orden según su contenido (el campo de tamaño de algunos modelos no es fiable).
"""
import os, struct, sys

KNOWN = {0x0003, 0x0005, 0x0100, 0x0102, 0x0200, 0x0300, 0x0400, 0x0500, 0x0600, 0x0700, 0x0800, 0x0900, 0x0A00,
         0x0B00, 0x0C00, 0x0D00, 0x0E00, 0x0F00, 0x1000, 0x1100, 0x1200, 0x1300, 0x1400, 0x1500, 0x1600, 0x1700,
         0x1800, 0x1900, 0x1A00, 0x1B00, 0x1C00, 0x1D00, 0x1E00, 0x1F00, 0x2000, 0x2100, 0x2200, 0x2300, 0x2400}
SEC_NAMES = {0x0100: 'object', 0x0200: 'material', 0x0300: 'texture', 0x0400: 'clut', 0x0800: 'model',
             0x0900: 'clump', 0x0005: 'stream'}


class R:
    def __init__(self, d, p=0): self.d, self.p = d, p
    def u8(self): v = self.d[self.p]; self.p += 1; return v
    def i16(self): v = struct.unpack_from('<h', self.d, self.p)[0]; self.p += 2; return v
    def u16(self): v = struct.unpack_from('<H', self.d, self.p)[0]; self.p += 2; return v
    def i32(self): v = struct.unpack_from('<i', self.d, self.p)[0]; self.p += 4; return v
    def u32(self): v = struct.unpack_from('<I', self.d, self.p)[0]; self.p += 4; return v
    def f32(self): v = struct.unpack_from('<f', self.d, self.p)[0]; self.p += 4; return v
    def align4(self): self.p = (self.p + 3) & ~3


class SubModel:
    def __init__(self):
        self.mat = 0; self.parent = -1; self.normals = []; self.verts = []; self.tris = []; self.uvs = []; self.deform = False


class CCS:
    def __init__(self, data):
        self.d = data; r = R(data)
        t, n = r.u32(), r.u32(); assert t & 0xFFFF == 1 and data[8:12] == b'CCSF'
        self.name = data[12:44].split(b'\0')[0].decode(); r.p = 44; self.version = r.u32(); r.p = 8 + n * 4
        t, n = r.u32(), r.u32(); assert t & 0xFFFF == 2
        # marca de sección: 0xCCCC en PS2 (y en Last Recode 0x124); en 0x131 la parte alta varía por sección: no se mira
        self.tag = None if self.version >= 0x131 else t >> 16
        nfiles, nobjs = r.u32(), r.u32()
        self.files = [data[r.p + 32 * i: r.p + 32 * i + 32].split(b'\0')[0].decode('latin1') for i in range(nfiles)]
        r.p += 32 * nfiles
        self.objnames = []
        for i in range(nobjs):
            e = data[r.p: r.p + 32]; r.p += 32
            self.objnames.append(e[:30].split(b'\0')[0].decode('latin1'))
        t, n = r.u32(), r.u32(); assert t & 0xFFFF == 3, hex(t); r.p += n * 4
        self.objects, self.materials, self.textures, self.cluts, self.models, self.clumps = {}, {}, {}, {}, {}, []
        self.warn = []
        while r.p + 8 <= len(data):
            off = r.p; t, n = r.u32(), r.u32()
            st = t & 0xFFFF
            if (self.tag is not None and t >> 16 != self.tag) or st == 0x0005 or st not in KNOWN: break
            sid = r.u32(); end = off + 8 + n * 4
            if st == 0x0100:
                self.objects[sid] = dict(parent=r.i32(), model=r.i32(), shadow=r.i32())
            elif st == 0x0200:
                self.materials[sid] = dict(tex=r.i32(), alpha=r.f32(), off=(r.f32(), r.f32()))
            elif st == 0x0400:
                r.u32(); r.u32(); r.u32(); nc = r.u32()
                self.cluts[sid] = [tuple(data[r.p + 4 * i: r.p + 4 * i + 4]) for i in range(nc)]
            elif st == 0x0300:
                self.textures[sid] = self._texture(r)
            elif st == 0x0900:
                self.clumps.append(self._clump(r, sid))
            elif st == 0x0800:
                self.models[sid] = self._model(r, sid)
            # El campo de tamaño no siempre cuadra (texturas, sombras): gana donde empieza una sección válida.
            ok = lambda p: p + 4 <= len(data) and self._is_sec(struct.unpack_from('<I', data, p)[0])
            if r.p != end and ok(r.p) and not ok(end):
                end = r.p
            elif not ok(end) and end < len(data):
                self.warn.append(f'sección {st:#06x} {self.objnames[sid]} en {off:#x}: ni lectura ({r.p:#x}) ni tamaño ({end:#x}) cuadran')
            r.p = end

    def _is_sec(self, t):
        return (t & 0xFFFF) in KNOWN and (self.tag is None or t >> 16 == self.tag)

    def _texture(self, r):
        clut, blit = r.i32(), r.i32(); flags, ttype, mips, _ = r.u8(), r.u8(), r.u8(), r.u8()
        w, h = r.u8(), r.u8(); r.u16()
        if w == 0xFF or h == 0xFF: w, h = r.i16(), r.i16()
        else: w, h = 1 << w, 1 << h; r.u32()
        size = r.u32() << 2
        pix = self.d[r.p: r.p + size]; r.p += size
        if pix[:4] == b'btx' + bytes(1):
            # Last Recode: textura DXT1 con cabecera "btx" de 88 bytes (ancho y alto u16 en +8), guardada de cabeza
            w, h = struct.unpack_from('<HH', pix, 8)
            return dict(clut=None, type=ttype, w=w, h=h, pix=pix, dxt=pix[88:88 + w * h // 2])
        for _ in range(mips): r.u32(); r.p += r.u32() << 2
        return dict(clut=clut, type=ttype, w=w, h=h, pix=pix)

    def _uvs(self, r, nv):
        """UV por vértice: i16/256 en PS2; en Last Recode (0x131) i32/65536 (los mismos valores con 8 bits más)"""
        if self.version >= 0x131:
            return [(r.i32() / 65536.0, r.i32() / 65536.0) for _ in range(nv)]
        return [(r.i16() / 256.0, r.i16() / 256.0) for _ in range(nv)]

    def _infl(self, p):
        """Una influencia de hueso de un vértice deformable: (x, y, z, peso/256, es la última, índice en la lookup)"""
        if self.version >= 0x131:
            x, y, z, wt, last, bone = struct.unpack_from('<hhhHHH', self.d, p)
            return x, y, z, wt, last != 0, bone
        x, y, z, prm = struct.unpack_from('<hhhH', self.d, p)
        return x, y, z, prm & 0x1FF, ((prm >> 9) & 1) != 0, prm >> 10

    def _clump(self, r, sid):
        n = r.u32(); ids = [r.u32() for _ in range(n)]
        pose = [([r.f32() for _ in range(3)], [r.f32() for _ in range(3)], [r.f32() for _ in range(3)]) for _ in range(n)]
        return dict(id=sid, nodes=ids, pose=pose)

    def _strip(self, r, sm, nv):
        """Normales (3 bytes) + bandera de tira por vértice: bandera 0 = cierra un triángulo con los dos anteriores."""
        k = 0
        for i in range(nv):
            sm.normals.append(struct.unpack_from('<bbb', self.d, r.p)); r.p += 3; flag = r.u8()
            if flag == 0:
                sm.tris.append((i, i - 1, i - 2) if k % 2 == 0 else (i - 2, i - 1, i)); k += 1
            else:
                k = 0

    def _model(self, r, sid):
        scale = r.f32(); mtype = r.i16() & 0xFFFE; nsub = r.i16(); r.i16(); r.i16(); nlook = r.i32()
        r.f32(); r.f32()
        q = scale / 256.0
        m = dict(type=mtype, scale=scale, subs=[], lookup=None)
        if nsub <= 0: return m
        if mtype == 0x1004 or mtype == 0x4:
            look = list(self.d[r.p: r.p + nlook]); r.p += nlook; r.align4(); m['lookup'] = look
            for _ in range(nsub - 1):
                sm = SubModel(); sm.mat = r.i32(); nv = r.i32(); r.i32(); sm.parent = look[r.i32()]
                sm.verts = [(((r.i16() * q, r.i16() * q, r.i16() * q), sm.parent, 1.0),) for _ in range(nv)]
                if r.p % 4 == 2: r.p += 2
                self._strip(r, sm, nv)
                sm.uvs = self._uvs(r, nv)
                m['subs'].append(sm)
            sm = SubModel(); sm.deform = True; sm.mat = r.i32(); nv = r.i32(); nw = r.i32()
            uvw = 8 if self.version >= 0x131 else 4
            # influencia: PS2 8 bytes (x, y, z, u16 = peso 9 bits | fin bit 9 | hueso << 10); Last Recode 0x131
            # 12 bytes (x, y, z, peso u16, fin u16, hueso u16)
            rec = 12 if self.version >= 0x131 else 8
            vb = r.p; tb = vb + nw * rec; ub = tb + nw * 4; vc = 0; k = 0
            for i in range(nv):
                p = vb + vc * rec; tp = tb + vc * 4      # normal y bandera: en el primer registro del vértice
                x, y, z, wt, last, bone = self._infl(p)
                infl = [((x * q, y * q, z * q), look[bone], wt / 256.0)]
                if not last:
                    vc += 1; x, y, z, wt, last, bone = self._infl(p + rec)
                    infl.append(((x * q, y * q, z * q), look[bone], wt / 256.0))
                sm.verts.append(tuple(infl))
                if os.environ.get('CCS_TLAST'): tp = tb + vc * 4
                sm.normals.append(struct.unpack_from('<bbb', self.d, tp)); flag = self.d[tp + 3]
                if flag == 0:
                    sm.tris.append((i, i - 1, i - 2) if k % 2 == 0 else (i - 2, i - 1, i)); k += 1
                else:
                    k = 0
                if uvw == 8:
                    u, v = struct.unpack_from('<ii', self.d, ub + i * 8); sm.uvs.append((u / 65536.0, v / 65536.0))
                else:
                    u, v = struct.unpack_from('<hh', self.d, ub + i * 4); sm.uvs.append((u / 256.0, v / 256.0))
                vc += 1
            r.p = ub + nv * uvw
            m['subs'].append(sm)
        elif mtype in (0x0, 0x2, 0x200, 0x1000, 0x1200, 0x400, 0x600):
            for _ in range(nsub):
                sm = SubModel(); sm.parent = r.i32(); sm.mat = r.i32(); nv = r.i32()
                sm.verts = [(((r.i16() * q, r.i16() * q, r.i16() * q), sm.parent, 1.0),) for _ in range(nv)]
                if r.p % 4 == 2: r.p += 2
                self._strip(r, sm, nv)
                if mtype < 0x4 or mtype in (0x1000, 0x400): r.p += 4 * nv          # colores de vértice
                if mtype not in (0x600, 0x400): sm.uvs = self._uvs(r, nv)
                m['subs'].append(sm)
        elif mtype == 0x8:
            for _ in range(nsub):
                nv, nt = r.i32(), r.i32(); r.p += nv * 8 + nt * 4
        else:
            self.warn.append(f'modelo {self.objnames[sid]}: tipo desconocido {mtype:#x}')
        return m


if __name__ == '__main__':
    c = CCS(open(sys.argv[1], 'rb').read())
    print(c.name, hex(c.version), 'archivos', c.files)
    print('objetos', len(c.objects), 'materiales', len(c.materials), 'texturas', len(c.textures), 'cluts', len(c.cluts),
          'modelos', len(c.models), 'clumps', [len(k['nodes']) for k in c.clumps])
    for sid, m in c.models.items():
        if m['subs']:
            print(f"  {c.objnames[sid]:28} tipo {m['type']:#06x} escala {m['scale']:.3f} subs {len(m['subs'])} "
                  f"verts {sum(len(s.verts) for s in m['subs'])} tris {sum(len(s.tris) for s in m['subs'])}")
    for sid, t in c.textures.items(): print('  tex', c.objnames[sid], t['type'], t['w'], t['h'], len(t['pix']), 'clut', c.objnames[t['clut']] if t['clut'] is not None and t['clut'] < len(c.objnames) else t['clut'])
    for w in c.warn[:10]: print('AVISO', w)
