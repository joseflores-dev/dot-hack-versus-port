"""Exporta un CCS de G.U. a OBJ (pose de reposo) + texturas PNG.
uso: python export_obj.py <archivo.ccs> <carpeta_salida> [--order XYZ] [--swz]"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
from PIL import Image
import ccs

args = [a for a in sys.argv[1:] if not a.startswith('--')]
ORDER = next((a.split('=')[1] for a in sys.argv if a.startswith('--order=')), 'ZYX')
SWZ = '--swz' in sys.argv          # G.U. guarda la paleta ya en orden: sin el intercalado CSM1
SIGN = -1.0 if '--neg' in sys.argv else 1.0
src, out = args[0], args[1]
os.makedirs(out, exist_ok=True)
c = ccs.CCS(open(src, 'rb').read())


def rot(axis, deg):
    a = np.radians(deg) * SIGN; s, k = np.sin(a), np.cos(a); m = np.eye(4)
    i, j = {'X': (1, 2), 'Y': (2, 0), 'Z': (0, 1)}[axis]
    m[i, i] = k; m[i, j] = s; m[j, i] = -s; m[j, j] = k      # convención de vector fila: v' = v @ m
    return m


def local(p, r, s):
    m = np.diag([s[0], s[1], s[2], 1.0])
    for ax in ORDER: m = m @ rot(ax, r['XYZ'.index(ax)])
    t = np.eye(4); t[3, :3] = p
    return m @ t


# Matrices de mundo: cada nodo del clump se encadena con su objeto padre.
k = c.clumps[0]
idx = {nid: i for i, nid in enumerate(k['nodes'])}
world = {}
def wm(i):
    if i in world: return world[i]
    nid = k['nodes'][i]; p, r, s = k['pose'][i]; m = local(p, r, s)
    par = c.objects.get(nid, {}).get('parent', -1)
    if par in idx: m = m @ wm(idx[par])
    world[i] = m; return m
for i in range(len(k['nodes'])): wm(i)

# Texturas: índice de 8 bits con paleta (en PS2 la paleta de 256 va intercalada en bloques de 8: CSM1).
def dxt1(data, w, h):
    """DXT1 (BC1) -> RGBA: bloques de 4x4, dos colores 565 y 2 bits por píxel."""
    out = np.zeros((h, w, 4), np.uint8)
    blocks = np.frombuffer(data[: w * h // 2], np.uint8).reshape(h // 4, w // 4, 8)
    c0 = blocks[..., 0].astype(np.uint32) | (blocks[..., 1].astype(np.uint32) << 8)
    c1 = blocks[..., 2].astype(np.uint32) | (blocks[..., 3].astype(np.uint32) << 8)
    def rgb(c):
        return np.stack([(c >> 11) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31], -1).astype(np.int32)
    a, b = rgb(c0), rgb(c1)
    four = (c0 > c1)[..., None]
    pal = np.stack([a, b, np.where(four, (2 * a + b) // 3, (a + b) // 2), np.where(four, (a + 2 * b) // 3, 0)], 2)
    alpha = np.stack([np.full(c0.shape, 255), np.full(c0.shape, 255), np.full(c0.shape, 255),
                      np.where(c0 > c1, 255, 0)], 2)
    bits = blocks[..., 4:8].astype(np.uint32)
    idx = bits[..., 0] | (bits[..., 1] << 8) | (bits[..., 2] << 16) | (bits[..., 3] << 24)
    for y in range(4):
        for x in range(4):
            k = ((idx >> (2 * (4 * y + x))) & 3).astype(np.intp)
            col = np.take_along_axis(pal, k[..., None, None].repeat(3, -1), 2)[..., 0, :]
            al = np.take_along_axis(alpha, k[..., None], 2)[..., 0]
            out[y::4, x::4, :3] = col
            out[y::4, x::4, 3] = al
    return out


texfile = {}
for sid, t in c.textures.items():
    if t.get('dxt'):   # Last Recode: DXT1, guardada de cabeza
        im = Image.fromarray(dxt1(t['dxt'], t['w'], t['h'])).transpose(Image.FLIP_TOP_BOTTOM)
        name = c.objnames[sid] + '.png'; im.save(os.path.join(out, name)); texfile[sid] = name
        continue
    pal = c.cluts.get(t['clut'])
    if pal is None: continue
    if SWZ and len(pal) == 256:
        pal = [pal[(i & ~0x18) | ((i & 0x08) << 1) | ((i & 0x10) >> 1)] for i in range(256)]
    rgba = [(r_, g_, b_, min(255, a_ * 2)) for (r_, g_, b_, a_) in pal]
    w, h = t['w'], t['h']
    if t['type'] == 0x13: ids = t['pix'][: w * h]
    else: ids = bytes(v for b in t['pix'][: w * h // 2] for v in (b & 15, b >> 4))
    im = Image.new('RGBA', (w, h)); im.putdata([rgba[i] for i in ids])
    name = c.objnames[sid] + '.png'; im.save(os.path.join(out, name)); texfile[sid] = name

# Malla: posición en mundo = suma ponderada de cada influencia (posición local al hueso * matriz del hueso).
U = 1.0 / 16.0
base = os.path.splitext(os.path.basename(src))[0]
mtl = open(os.path.join(out, base + '.mtl'), 'w')
obj = open(os.path.join(out, base + '.obj'), 'w')
obj.write(f'mtllib {base}.mtl\n')
for mid, m in c.materials.items():
    mtl.write(f'newmtl {c.objnames[mid]}\n')
    if m['tex'] in texfile: mtl.write(f'map_Kd {texfile[m["tex"]]}\n')
vbase = 1
for i, nid in enumerate(k['nodes']):
    o = c.objects.get(nid, {}); mdl = c.models.get(o.get('model', 0))
    if not mdl or not mdl['subs']: continue
    rigid_bone = i
    for sm in mdl['subs']:
        obj.write(f'g {c.objnames[o["model"]]}\nusemtl {c.objnames[sm.mat] if sm.mat in c.materials else "none"}\n')
        for v in sm.verts:
            acc = np.zeros(3)
            for pos, bone, wgt in v:
                b = bone if (mdl['lookup'] is not None) else rigid_bone
                acc += wgt * (np.array([pos[0] * U, pos[1] * U, pos[2] * U, 1.0]) @ world[b])[:3]
            obj.write('v %.4f %.4f %.4f\n' % (acc[0], acc[2], -acc[1]))   # el juego usa Z arriba; el OBJ, Y arriba
        for uv in sm.uvs: obj.write('vt %.5f %.5f\n' % (uv[0], 1 - uv[1]))
        for t in sm.tris:
            if min(t) < 0: continue
            a, b_, cc = (x + vbase for x in t)
            obj.write(f'f {a}/{a} {b_}/{b_} {cc}/{cc}\n')
        vbase += len(sm.verts)
print('ok', out, base, 'huesos', len(k['nodes']), 'texturas', len(texfile))
