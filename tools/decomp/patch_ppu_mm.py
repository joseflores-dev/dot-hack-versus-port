"""Chequeo inline de escrituras más corto en recomp/ppu_recomp.h (fase 33).

- los vigías de depuración g_barrier_sync_watch | g_null_sweep_hi en una sola palabra, g_ppu_store_watch
  (ppu_loader.cpp / spu_channels.c la mantienen);
- el bitmap de líneas reservadas por SPU en la copia _mm (spu_coh_bitmap_publish, spu_coherency.c: el arreglo es
  estático), sin cargar el puntero global ni comprobar NULL en cada escritura;
- sin la rama de alineación en las cargas/escrituras inline (arm64 y x86-64 aceptan desalineadas; dejaba un tst).
Idempotente. uso: python tools/decomp/patch_ppu_mm.py recomp/ppu_recomp.h
"""
import sys

p = sys.argv[1]
s = open(p, encoding='utf-8').read()
if 'g_ppu_store_watch' in s:
    print('ya aplicado'); sys.exit(0)


def rep(a, b):
    global s
    assert s.count(a) == 1, a[:70]
    s = s.replace(a, b)


rep("extern const unsigned char* g_spu_coh_bits;   /* SPU-reserved lines, NULL until the first */",
    "extern const unsigned char* g_spu_coh_bits;   /* SPU-reserved lines, NULL until the first */\n"
    "/* HV: g_barrier_sync_watch | g_null_sweep_hi in one word (one load per store); kept by whoever arms them. */\n"
    "extern uint32_t             g_ppu_store_watch;\n"
    "/* HV: the reserved-line bitmap itself (static, never moves): published into the _mm copy. */\n"
    "const unsigned char* spu_coh_bitmap_publish(void);")
rep("typedef struct ppu_mm { uint8_t* vb; int64_t ld_span, st_span; uint32_t poll; } ppu_mm;",
    "typedef struct ppu_mm { uint8_t* vb; int64_t ld_span, st_span; uint32_t poll; const unsigned char* coh; } ppu_mm;")
rep("    m.poll = ppu_hle_inject_base + 0x2008u;   /* the gcm ref-poll word: vm_read32 */\n",
    "    m.poll = ppu_hle_inject_base + 0x2008u;   /* the gcm ref-poll word: vm_read32 */\n"
    "    m.coh = spu_coh_bitmap_publish();\n")
rep("""        __builtin_expect((g_barrier_sync_watch | g_null_sweep_hi) != 0, 0))
        return 0;
    const unsigned char* bits = g_spu_coh_bits;
    if (bits) { const uint32_t line = ea >> 7; if ((bits[line >> 3] >> (line & 7)) & 1u) return 0; }
    return 1;""", """        __builtin_expect(g_ppu_store_watch != 0, 0))
        return 0;
    const uint32_t line = ea >> 7;   /* m->coh is set whenever st_span is not 0 (ppu_mm_compute) */
    return !((m->coh[line >> 3] >> (line & 7)) & 1u);""")
for T, sz in (('uint16_t', 1), ('uint32_t', 3), ('uint64_t', 7)):
    rep(f"        if (!(ea & {sz}u)) v = __atomic_load_n((const {T}*)(m->vb + ea), __ATOMIC_RELAXED);\n"
        f"        else memcpy(&v, m->vb + ea, {sz + 1});\n",
        f"        v = __atomic_load_n((const {T}*)(m->vb + ea), __ATOMIC_RELAXED);\n")
    rep(f"        if (!(ea & {sz}u)) __atomic_store_n(({T}*)(m->vb + ea), be, __ATOMIC_RELAXED);\n"
        f"        else memcpy(m->vb + ea, &be, {sz + 1});\n",
        f"        __atomic_store_n(({T}*)(m->vb + ea), be, __ATOMIC_RELAXED);\n")
open(p, 'w', encoding='utf-8', newline='\n').write(s)
print('aplicado')
