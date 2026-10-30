"""Inserta en el código generado por el lifter (recomp/, fuera del repo) las llamadas a los ganchos del port que
usan los mods de personajes (hackversus/stubs.cpp). Idempotente: lo corren build_apk.sh y build_win.sh.
  hv_chr_path_hook  tras el sprintf de "data/chr/%schrpsn.xfbin" en func_0004BB04 (crear un luchador)
  hv_color_hook     al dibujar el selector de color del menú de personaje (func_002865F0)
uso: python tools/apply_recomp_hooks.py [carpeta recomp]"""
import os
import sys

recomp = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'recomp')

HOOKS = [
    ('ppu_recomp_001.cpp',
     b'extern "C" void hv_chr_path_hook(uint32_t buf, uint32_t player);   /* port: model file of a character (mods) */\n',
     b'ctx->lr = 0x0004BC84; func_006E28F0(ctx); DRAIN_TRAMPOLINE(ctx);',
     b' hv_chr_path_hook((uint32_t)ctx->gpr[29], (uint32_t)ctx->gpr[26]);  /* port: mods (hackversus/stubs.cpp) */'),
    ('ppu_recomp_020.cpp',
     b'extern "C" void hv_color_hook(uint32_t setting, uint32_t widget);   /* port: colour selector (mods) */\n',
     b'        ctx->gpr[9] = vm_read32(ctx->gpr[31] + 0x54);\n        ctx->gpr[29] = ctx->gpr[30] + (int64_t)(0xA8);',
     None),
]

for name, decl, anchor, call in HOOKS:
    p = os.path.join(recomp, name)
    s = open(p, 'rb').read()
    if decl in s:
        print('%s: ya tiene el gancho' % name)
        continue
    assert s.count(anchor) == 1, '%s: no se encontró el punto de inserción (¿otra versión del lifter?)' % name
    if call is not None:
        s = s.replace(anchor, anchor + call)
    else:   # hv_color_hook va en su propia línea, entre las dos del ancla
        first, second = anchor.split(b'\n', 1)
        s = s.replace(anchor, first + b'\n        hv_color_hook((uint32_t)ctx->gpr[9], (uint32_t)ctx->gpr[31]);  /* port: colour selector (hackversus/stubs.cpp) */\n' + second)
    s = s.replace(b'#include "ppu_recomp.h"\n', b'#include "ppu_recomp.h"\n' + decl, 1)
    open(p, 'wb').write(s)
    print('%s: gancho insertado' % name)
