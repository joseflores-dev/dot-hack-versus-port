"""vperm / vsldoi del código PPU ya traducido (recomp/*.cpp) como llamadas a ppu_vmx.h (vmx_vperm, VMX_VSLDOI).

El lifter emite cada vperm como un bucle de 16 bytes y cada vsldoi con un temporal de 32 bytes en la pila; en arm64
eso eran cargas y guardados de a un byte. vmx_vperm (NEON tbl / SSE pshufb) y VMX_VSLDOI (vext / palignr) dan los
mismos bytes (probado contra el bucle original). Idempotente: lo ya reemplazado no vuelve a coincidir.
uso: python tools/decomp/vmx_lower.py recomp/
"""
import glob
import os
import re
import sys

VPERM = re.compile(r'\{ uint8_t\* a = \(uint8_t\*\)&ctx->vr\[(\d+)\]; uint8_t\* b = \(uint8_t\*\)&ctx->vr\[(\d+)\]; '
                   r'uint8_t\* c = \(uint8_t\*\)&ctx->vr\[(\d+)\]; uint8_t\* d = \(uint8_t\*\)&ctx->vr\[(\d+)\]; '
                   r'uint8_t tmp\[16\]; for \(int i = 0; i < 16; i\+\+\) \{ uint8_t sel = c\[i\] & 0x1F; '
                   r'tmp\[i\] = \(sel < 16\) \? a\[sel\] : b\[sel - 16\]; \} memcpy\(d, tmp, 16\); \}')
VSLDOI = re.compile(r'\{ uint8_t tmp\[32\]; memcpy\(tmp, &ctx->vr\[(\d+)\], 16\); memcpy\(tmp\+16, &ctx->vr\[(\d+)\], 16\); '
                    r'memcpy\(&ctx->vr\[(\d+)\], tmp \+ (\d+), 16\); \}')


def main():
    d = sys.argv[1]
    np = ns = 0
    for p in sorted(glob.glob(os.path.join(d, 'ppu_recomp_*.cpp'))):
        s = open(p, encoding='utf-8', errors='surrogateescape').read()
        s, a = VPERM.subn(lambda m: f'vmx_vperm(&ctx->vr[{m[4]}], &ctx->vr[{m[1]}], &ctx->vr[{m[2]}], &ctx->vr[{m[3]}]);', s)
        s, b = VSLDOI.subn(lambda m: f'VMX_VSLDOI(&ctx->vr[{m[3]}], &ctx->vr[{m[1]}], &ctx->vr[{m[2]}], {m[4]});', s)
        if a or b:
            open(p, 'w', encoding='utf-8', errors='surrogateescape', newline='\n').write(s)
        np += a; ns += b
    print(f'vperm: {np}, vsldoi: {ns}')


if __name__ == '__main__':
    main()
