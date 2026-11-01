"""Fusión comparación + salto sobre el código PPU ya traducido (recomp/*.cpp), con análisis de vida de los campos de CR.

El lifter traduce cada cmpwi/cmplw/fcmpu... como una escritura de su campo en ctx->cr (con el bit SO de xer) y cada
salto condicional como una lectura de ese campo. Cuando el único lector de esa escritura es el salto que la sigue
(y el campo está muerto después del salto, por los dos caminos), la comparación se borra y el salto pasa a comparar
los operandos directamente: `if ((int64_t)(int32_t)ctx->gpr[3] < (int64_t)0) goto loc_X;`.

Seguridad (conservador en todo lo que no entiende):
  - vida por función sobre un grafo de líneas (goto/switch/return/trampolín; caer a la línea siguiente salvo
    terminador claro: sobran aristas, nunca faltan). Una etiqueta destino que no está en la función -> función intacta.
  - lecturas: (ctx->cr >> N) & M, (ctx->cr >> (31 - K)) & 1; cualquier otro uso de ctx->cr lee TODOS los campos.
    Escrituras completas: el bloque de comparación, (ctx->cr & ~(0xFu << N)), máscaras hex con el nibble en 0, mtcr.
  - salidas: `return;` (blr) deja vivos CR2-CR4 (no volátiles); cualquier otra salida (trampolín, switch, stub,
    llamada + return) deja vivos todos. Las llamadas leen CR2-CR4.
  - entre la comparación y el salto solo se aceptan líneas sin ctx->cr, sin control de flujo, sin `ctx` suelto
    (llamadas) y que no escriban los registros de los operandos; máximo 6 líneas.
  - comparaciones enteras: solo bits LT/GT/EQ (SO viene de xer: se rechaza). FP: LT/GT/EQ/UN con NaN correcto.

Modo --poison: en vez de borrar la comparación escribe en el campo el valor con LT/GT/EQ invertidos. Si el análisis
de vida se equivocara, quien lea ese campo toma el camino contrario: sirve para validar antes del modo normal.

uso: python tools/decomp/fuse_cr.py recomp/ [--poison] [--dry]
"""
import os
import re
import sys

NONVOL = (1 << 2) | (1 << 3) | (1 << 4)     # bits por campo: bit f = CRf
ALL = 0xFF

CMP_RE = re.compile(
    r"^\{ (?P<ta>int64_t|uint64_t|double) a = (?P<a>[^;]+); (?P<tb>int64_t|uint64_t|double) b = (?P<b>[^;]+); "
    r"uint32_t cr_val = (?P<val>[^;]+); "
    r"ctx->cr = \(ctx->cr & ~\(0xFu << (?P<sh>\d+)\)\) \| \(cr_val << (?P=sh)\); \}$")
INT_VAL = "((a < b) ? 8 : (a > b) ? 4 : 2) | ((ctx->xer >> 31) & 1u)"
FP_VAL = "(a != a || b != b) ? 1 : (a < b) ? 8 : (a > b) ? 4 : 2"
BR_RE = re.compile(r"^if \((?P<neg>\(!)?\(\(ctx->cr >> (?P<sh>\d+)\) & (?P<m>\d+)\)\)(?(neg)\)) (?P<rest>.+)$")
FUNC_RE = re.compile(r"^void (func_\w+)\(ppu_context\* (?:__restrict )?ctx\) \{$")
LABEL_RE = re.compile(r"^(loc_\w+):")
GOTO_RE = re.compile(r"goto (loc_\w+);")
REGS_RE = re.compile(r"ctx->(gpr|fpr)\[(\d+)\]")
BARE_CTX = re.compile(r"\bctx\b(?!->)")

W_FIELD = re.compile(r"ctx->cr\s*=\s*\(ctx->cr & ~\(0xFu\s*<<\s*(\d+)\)\)")
W_HEX = re.compile(r"ctx->cr\s*=\s*\(ctx->cr & (~?)(0x[0-9A-Fa-f]+)u\)")
W_BIT = re.compile(r"ctx->cr\s*=\s*\(ctx->cr & ~\(1u << \(31 - (\d+)\)\)\)")
W_ALL = re.compile(r"ctx->cr\s*=\s*\(uint32_t\)ctx->gpr\[\d+\];")
R_SHM = re.compile(r"\(ctx->cr >> (\d+)\) & (0x[0-9A-Fa-f]+|\d+)\b")
R_BIT = re.compile(r"\(ctx->cr >> \(31 - (\d+)\)\) & 1\b")


def fields_of_bits(mask32):
    f = 0
    for k in range(8):
        if mask32 & (0xF << ((7 - k) * 4)):
            f |= 1 << k
    return f


def field_of_shift(sh):
    return 1 << (7 - sh // 4)


def use_def(s):
    """(use, def) de campos de CR para una línea (ya sin espacios iniciales)."""
    if "ctx->cr" not in s:
        u = NONVOL if BARE_CTX.search(s) and not s.startswith("PPU_MM") else 0
        return u, 0
    m = CMP_RE.match(s)
    if m and m.group("val") in (INT_VAL, FP_VAL):
        return 0, field_of_shift(int(m.group("sh")))
    use = dfn = 0
    t = s
    if W_ALL.search(t):
        dfn |= ALL
        t = W_ALL.sub("", t)
    for mm in list(W_FIELD.finditer(t)):
        dfn |= field_of_shift(int(mm.group(1)))
    t = W_FIELD.sub("", t)
    for mm in list(W_BIT.finditer(t)):
        use |= fields_of_bits(1 << (31 - int(mm.group(1))))          # escritura parcial: cuenta como lectura
    t = W_BIT.sub("", t)
    for mm in list(W_HEX.finditer(t)):
        v = int(mm.group(2), 16)
        keep = (~v & 0xFFFFFFFF) if mm.group(1) else v
        for k in range(8):
            nib = (keep >> ((7 - k) * 4)) & 0xF
            if nib == 0:
                dfn |= 1 << k
            elif nib != 0xF:
                use |= 1 << k
    t = W_HEX.sub("", t)
    for mm in list(R_BIT.finditer(t)):
        use |= fields_of_bits(1 << (31 - int(mm.group(1))))
    t = R_BIT.sub("", t)
    for mm in list(R_SHM.finditer(t)):
        sh, msk = int(mm.group(1)), int(mm.group(2), 0)
        use |= fields_of_bits((msk << sh) & 0xFFFFFFFF)
    t = R_SHM.sub("", t)
    if "ctx->cr" in t:
        use |= ALL
    if BARE_CTX.search(t):
        use |= NONVOL
    # un campo leído y escrito en la misma línea: la lectura va primero (use) y la escritura lo mata (def)
    return use, dfn


def strip_comment(s):
    i = s.find("/*")
    return s[:i].rstrip() if i >= 0 else s


def terminates(s):
    if s.startswith("if ("):
        return False
    if s.startswith("goto "):
        return True
    return s.endswith("return;") or s.endswith("return; }")


def exit_live(s):
    if s == "return;" or (s.startswith("if (") and s.endswith(") return;") and "ctx)" not in s):
        return NONVOL
    return ALL


def cond_of(m, mask, neg):
    ta, tb, a, b = m.group("ta"), m.group("tb"), m.group("a"), m.group("b")
    A, B = f"({ta})({a})", f"({tb})({b})"
    if ta == "double":
        c = {8: f"{A} < {B}", 4: f"{A} > {B}", 2: f"{A} == {B}", 1: f"({A} != {A} || {B} != {B})"}.get(mask)
    else:
        c = {8: f"{A} < {B}", 4: f"{A} > {B}", 2: f"{A} == {B}"}.get(mask)
    if c is None:
        return None
    return f"(!({c}))" if neg else f"({c})"


def process(name, body, poison, stats):
    """body: lista de líneas (con indentación) entre la firma y la llave final. Devuelve la nueva lista."""
    n = len(body)
    st = [strip_comment(l.strip()) for l in body]
    labels = {}
    for i, s in enumerate(st):
        m = LABEL_RE.match(s)
        if m:
            labels[m.group(1)] = i
    succ, use, dfn, ex = [], [], [], []
    for i, s in enumerate(st):
        u, d = use_def(s)
        use.append(u)
        dfn.append(d)
        nx = []
        for g in GOTO_RE.findall(s):
            if g not in labels:
                stats["func_skip"] += 1
                return body
            nx.append(labels[g])
        if not terminates(s) and i + 1 < n:
            nx.append(i + 1)
        succ.append(nx)
        e = 0
        if "return" in s:
            e = exit_live(s)
        if not terminates(s) and i + 1 >= n:
            e = ALL                      # cae al final del cuerpo (lo resuelve el código de después)
        ex.append(e)
    live_in = [0] * n
    live_out = [0] * n
    changed = True
    while changed:
        changed = False
        for i in range(n - 1, -1, -1):
            o = ex[i]
            for j in succ[i]:
                o |= live_in[j]
            li = use[i] | (o & ~dfn[i])
            if o != live_out[i] or li != live_in[i]:
                live_out[i] = o
                live_in[i] = li
                changed = True
    out = list(body)
    drop = set()
    for i, s in enumerate(st):
        m = CMP_RE.match(s)
        if not m or m.group("val") not in (INT_VAL, FP_VAL):
            continue
        stats["cmp"] += 1
        sh = int(m.group("sh"))
        f = field_of_shift(sh)
        if not live_out[i] & f:
            stats["dead"] += 1
            if poison:
                out[i] = body[i].replace("ctx->cr = (ctx->cr & ~", "cr_val ^= 0xEu; ctx->cr = (ctx->cr & ~", 1)
            else:
                drop.add(i)
            continue
        regs = set(REGS_RE.findall(m.group("a") + " " + m.group("b")))
        j = i + 1
        ok = False
        while j < n and j <= i + 7:
            t = st[j]
            if not t or t.startswith("//"):
                j += 1
                continue
            mb = BR_RE.match(t)
            if mb:
                bsh, bm = int(mb.group("sh")), int(mb.group("m"))
                ok = bm == 1 and bsh // 4 == sh // 4 or (bsh == sh and bm in (1, 2, 4, 8))
                break
            if ("ctx->cr" in t or LABEL_RE.match(t) or "goto" in t or "return" in t or "switch" in t
                    or t.startswith("if") or t.startswith("#") or BARE_CTX.search(t)):
                break
            if any(re.search(rf"ctx->{k}\[{r}\]\s*(=(?!=)|[-+*/&|^]=|<<=|>>=|\+\+|--)", t) or
                   re.search(rf"(&|\+\+|--)\s*ctx->{k}\[{r}\]", t) for k, r in regs):
                break
            j += 1
        if not ok:
            stats["no_branch"] += 1
            continue
        mask = (1 << (bsh - sh)) if bm == 1 else bm       # (cr >> sh+k) & 1  ==  bit k del campo
        c = cond_of(m, mask, bool(mb.group("neg")))
        if c is None:
            stats["mask"] += 1
            continue
        # tras el salto el campo debe estar muerto por todos los caminos (la línea del salto solo lee este campo)
        if live_out[j] & f:
            stats["live_after"] += 1
            continue
        ind = body[j][:len(body[j]) - len(body[j].lstrip())]
        out[j] = f"{ind}if {c} {mb.group('rest')}"
        if poison:
            out[i] = body[i].replace("ctx->cr = (ctx->cr & ~", "cr_val ^= 0xEu; ctx->cr = (ctx->cr & ~", 1)
        else:
            drop.add(i)
        stats["fused"] += 1
    return [l for k, l in enumerate(out) if k not in drop]


def main():
    d = sys.argv[1]
    poison = "--poison" in sys.argv
    dry = "--dry" in sys.argv
    stats = dict(cmp=0, fused=0, dead=0, no_branch=0, mask=0, live_after=0, func_skip=0)
    for fn in sorted(os.listdir(d)):
        if not re.match(r"ppu_recomp_\d+\.cpp$", fn):
            continue
        p = os.path.join(d, fn)
        lines = open(p, encoding="utf-8", errors="surrogateescape").read().split("\n")
        res = []
        i = 0
        while i < len(lines):
            res.append(lines[i])
            if FUNC_RE.match(lines[i]):
                k = i + 1
                while k < len(lines) and lines[k] != "}":
                    k += 1
                res.extend(process(lines[i], lines[i + 1:k], poison, stats))
                i = k
                continue
            i += 1
        if not dry:
            open(p, "w", encoding="utf-8", errors="surrogateescape", newline="\n").write("\n".join(res))
        print(fn, dict(stats), flush=True)
    print("total", stats)


if __name__ == "__main__":
    main()
