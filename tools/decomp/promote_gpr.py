"""Registros GPR en variables locales dentro de cada función del código PPU ya traducido (recomp/*.cpp).

El lifter traduce cada registro como ctx->gpr[N]. ctx escapa (se pasa a cada llamada), así que después de cualquier
llamada que el compilador no ve por dentro -- incluidos los caminos lentos de vm_read*/vm_write* en cada acceso a
memoria del juego -- vuelve a leer de ctx los registros que usa y tiene que haber escrito antes los que cambió.
Con locales (_rN) esas lecturas/escrituras desaparecen: ctx->gpr solo se sincroniza donde ctx de verdad escapa.

Transformación por función (conservadora: la función queda intacta si aparece algo que no entiende):
  - entrada: `uint64_t _rN = ctx->gpr[N];` para cada N usado (las cargas muertas las borra el compilador).
  - cada ctx->gpr[N] del cuerpo pasa a _rN.
  - antes de cada punto donde ctx escapa -- llamada (func_X(ctx), ps3_indirect_call, lv2_syscall, ps3_hle_call),
    salida (return, trampolín g_trampoline_fn) -- se escriben en ctx->gpr los registros "sucios": escritos desde la
    última sincronización por algún camino (flujo de datos hacia adelante sobre el grafo de líneas: goto/switch/
    caída a la siguiente línea; sobran aristas, nunca faltan).
  - después de cada llamada se recargan todos los registros usados (macro _RL_ por función).
  - las salidas condicionales escriben solo en el camino que sale: `if (c) { ctx->gpr[3] = _r3; return; }`.
Se rechaza la función (queda igual) si: toma la dirección de un registro (&ctx->gpr), pasa ctx a algo que no es una
de las formas conocidas, o tiene un return/escape en una forma que no se reconoce.

uso: python tools/decomp/promote_gpr.py recomp/ [--dry]
"""
import os
import re
import sys

FUNC_RE = re.compile(r"^void (func_\w+)\(ppu_context\* (?:__restrict )?ctx\) \{$")
LABEL_RE = re.compile(r"^(loc_\w+):")
GOTO_RE = re.compile(r"goto (loc_\w+);")
GPR_RE = re.compile(r"ctx->gpr\[(\d+)\]")
WR_RE = re.compile(r"ctx->gpr\[(\d+)\]\s*(?:=(?!=)|[-+*/%&|^]=|<<=|>>=|\+\+|--)|(?:\+\+|--)\s*ctx->gpr\[(\d+)\]")
BARE_CTX = re.compile(r"\bctx\b(?!->)")

CALL_RE = re.compile(r"^(?P<pre>ctx->lr = 0x[0-9A-Fa-f]+; (?:func_\w+\(ctx\)|ps3_indirect_call\(ctx\)); "
                     r"DRAIN_TRAMPOLINE\(ctx\);)(?P<post>.*)$")
CALL_IF1_RE = re.compile(r"^(?P<pre>ctx->lr = 0x[0-9A-Fa-f]+; if \(1\) \{ ps3_indirect_call\(ctx\); "
                         r"DRAIN_TRAMPOLINE\(ctx\); \})(?P<post>.*)$")
SYSCALL = "lv2_syscall(ctx);"
HLE_RE = re.compile(r"^ps3_hle_call\(0x[0-9A-Fa-f]+u, ctx\); return;(?:\s*/\*.*\*/)?$")
TRAMP = "{ g_trampoline_fn = (void(*)(void*))"
TRAMP_RE = re.compile(r"\{ g_trampoline_fn = \(void\(\*\)\(void\*\)\)func_\w+; return; \}")
DEFAULT_IC = "default: ps3_indirect_call(ctx); return;"
IF_RET_RE = re.compile(r"^if \(.*\) return;$")
MTFSF_RE = re.compile(r"ppu_mtfsf\(ctx, ")   # solo toca fpscr: no es un escape de los GPR


def strip_comment(s):
    i = s.find("/*")
    return s[:i].rstrip() if i >= 0 else s


def writes(s):
    w = set()
    for m in WR_RE.finditer(s):
        w.add(int(m.group(1) or m.group(2)))
    return w


def kind_of(s):
    """Clase de la línea (sin indentación ni comentario): call/syscall/exit/plain, o None si no se entiende."""
    if CALL_RE.match(s) or CALL_IF1_RE.match(s):
        return "call"
    if s == SYSCALL:
        return "call"
    has_ret = "return" in s
    bare = BARE_CTX.search(MTFSF_RE.sub("", s))
    if HLE_RE.match(s):
        return "exit"
    if s.startswith("switch ((uint32_t)ctx->ctr)"):
        return "exit"   # sus salidas se reescriben; también tiene gotos
    if TRAMP in s:
        rest = TRAMP_RE.sub("", s)
        if BARE_CTX.search(rest) or ("return" in rest and rest.strip() not in ("", "return;")):
            return None
        return "exit"
    if bare:
        return None
    if has_ret:
        if s == "return;" or IF_RET_RE.match(s):
            return "exit"
        return None
    return "plain"


def terminates(s):
    if s.startswith("if ("):
        return False
    if s.startswith("goto "):
        return True
    if s.startswith("switch ("):
        return s.endswith("return;")
    return s == "return;" or HLE_RE.match(s) is not None or (s.startswith(TRAMP) and s.endswith("}"))


def flush(regs):
    return "".join(f"ctx->gpr[{r}] = _r{r}; " for r in sorted(regs))


def process(sig, body, stats):
    n = len(body)
    full = "\n".join(body)
    if "&ctx->gpr" in full or re.search(r"\b_r\d+\b", full):
        stats["skip_addr"] += 1
        return None
    used = sorted({int(x) for x in GPR_RE.findall(full)})
    if not used:
        stats["no_gpr"] += 1
        return None
    st = [strip_comment(l.strip()) for l in body]
    kinds = []
    for s in st:
        k = kind_of(s) if s else "plain"
        if k is None:
            stats["skip_form"] += 1
            if stats["skip_form"] <= 8:
                print("  forma no reconocida:", sig, "|", s[:160])
            return None
        kinds.append(k)
    labels = {}
    for i, s in enumerate(st):
        m = LABEL_RE.match(s)
        if m:
            labels[m.group(1)] = i
    succ = []
    for i, s in enumerate(st):
        e = set()
        for m in GOTO_RE.finditer(s):
            if m.group(1) not in labels:
                stats["skip_label"] += 1
                return None
            e.add(labels[m.group(1)])
        if not terminates(s) and i + 1 < n:
            e.add(i + 1)
        succ.append(e)
    wr = [writes(s) for s in st]
    # dirty_in por punto fijo (unión; entrada vacía)
    din = [set() for _ in range(n)]
    dout = [None] * n
    work = list(range(n))
    inq = [True] * n
    while work:
        i = work.pop()
        inq[i] = False
        o = set() if kinds[i] == "call" else (din[i] | wr[i])
        if dout[i] != o:
            dout[i] = o
            for j in succ[i]:
                if not o <= din[j]:
                    din[j] |= o
                    if not inq[j]:
                        inq[j] = True
                        work.append(j)
    out = []
    for i, line in enumerate(body):
        k = kinds[i]
        ind = line[:len(line) - len(line.lstrip())]
        t = GPR_RE.sub(lambda m: f"_r{m.group(1)}", line.strip())
        fl = flush(din[i] | wr[i])
        s = st[i]
        if k == "call":
            if s == SYSCALL:
                t = f"{fl}{t} _RL_"
            else:
                m = CALL_RE.match(t) or CALL_IF1_RE.match(t)
                t = f"{fl}{m.group('pre')} _RL_{m.group('post')}"
            stats["calls"] += 1
        elif k == "exit":
            if fl:
                if HLE_RE.match(s) or s == "return;":
                    t = fl + t
                elif s.startswith("switch ("):
                    t = t.replace(DEFAULT_IC, "default: " + fl + "ps3_indirect_call(ctx); return;")
                    t = t.replace(TRAMP, "{ " + fl + TRAMP[2:])
                    if t.endswith("} return;"):
                        t = t[:-len("return;")] + "{ " + fl + "return; }"
                elif TRAMP in s:
                    t = t.replace(TRAMP, "{ " + fl + TRAMP[2:])
                    if t.rstrip().endswith("} return;"):
                        t = t[:t.rfind("return;")] + "{ " + fl + "return; }"
                elif IF_RET_RE.match(s):
                    c = t.find(" return;", len("if ("))
                    t = t[:c] + " { " + fl + "return; }" + t[c + len(" return;"):]
                else:
                    raise AssertionError(s)
            stats["exits"] += 1
        out.append(ind + t)
    # salida por el final de la función (no debería pasar, pero si cae, sincroniza)
    last = n - 1
    while last >= 0 and not st[last]:
        last -= 1
    if last < 0 or not terminates(st[last]) or kinds[last] == "call":
        fl = flush(dout[last] if last >= 0 else set())
        if fl:
            out.append("        " + fl)
    # declaración: después de PPU_MM_SNAP (primera línea) para que ningún goto la salte
    decl = "        uint64_t " + ", ".join(f"_r{r} = ctx->gpr[{r}]" for r in used) + ";"
    snap = next((i for i, s in enumerate(st) if s), None)
    if snap is None or st[snap] != "PPU_MM_SNAP":
        stats["skip_snap"] += 1
        return None
    out.insert(snap + 1, decl)
    stats["promoted"] += 1
    rl = "#define _RL_ " + " ".join(f"_r{r} = ctx->gpr[{r}];" for r in used)
    return rl, out


def main():
    d = sys.argv[1]
    dry = "--dry" in sys.argv
    stats = dict(promoted=0, no_gpr=0, skip_addr=0, skip_form=0, skip_label=0, skip_snap=0, calls=0, exits=0)
    for fn in sorted(os.listdir(d)):
        if not re.match(r"ppu_recomp_\d+\.cpp$", fn):
            continue
        p = os.path.join(d, fn)
        src = open(p, encoding="utf-8", errors="surrogateescape").read()
        if "_RL_" in src:
            print(fn, "ya promovido")
            continue
        lines = src.split("\n")
        res = []
        i = 0
        while i < len(lines):
            if FUNC_RE.match(lines[i]):
                k = i + 1
                while k < len(lines) and lines[k] != "}":
                    k += 1
                r = process(lines[i], lines[i + 1:k], stats)
                if r is None:
                    res.extend(lines[i:k + 1])
                else:
                    rl, body = r
                    res.append(rl)
                    res.append(lines[i])
                    res.extend(body)
                    res.append("}")
                    res.append("#undef _RL_")
                i = k + 1
                continue
            res.append(lines[i])
            i += 1
        if not dry:
            open(p, "w", encoding="utf-8", errors="surrogateescape", newline="\n").write("\n".join(res))
        print(fn, dict(stats), flush=True)
    print("total", stats)


if __name__ == "__main__":
    main()
