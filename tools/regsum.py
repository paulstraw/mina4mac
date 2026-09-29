"""Register-preservation summaries for lifted functions (PLAN3 Phase 11, cheaper register sync).

For each function F this proves, from the lifted code, two facts a caller can use to skip reloads after `F_<addr>(c)`:
  - which of ebx/esi/edi/ebp F returns unchanged (c->r at return == c->r at entry), and
  - how many argument bytes F pops (`ret N`, reached with esp back at its entry value).
Anything unproven falls back to the full reload. The proof is an abstract interpretation over each function: registers
hold "the entry value of r", "entry esp + k" or unknown, and 4-byte stack slots (by offset from entry esp) hold what
push stored. Calls use the callee's summary; indirect calls and imports know nothing (callee-saved regs and esp become
unknown). Summaries are solved over the call graph from the optimistic "never returns" (recursion is sound by
induction on call depth).

Assumptions, as in any recompiler of compiler-generated code:
  - A saved-register stack slot is only written by push, or by a store at a known frame offset (esp or a register
    known to be entry esp + k, plus a constant). Stores through other pointers, indexed stores and pushes while esp is
    unknown never land in one (only a buffer overflow would), and a callee doesn't write its caller's frame outside the
    argument bytes it pops.
  - Where esp is unknown going forward only because indirect calls popped unknown amounts (vcalls: the callee pops its
    arguments), and every path from there to a `ret` moves it by known amounts, esp is what that `ret` needs
    (required()): otherwise the original program couldn't return either. A `ret` must also go through slot 0, still
    holding the return address. Esp lost any other way (`sub esp, eax`, `and esp, -8`, a direct call to a helper like
    __SEH_prolog4 that returns with esp moved) stays unknown.

  uv run tools/regsum.py [module]    print summary statistics (and write the cache build/<module>/regsum.pkl)
"""
import hashlib
import os
import pickle
import re
import sys
from pathlib import Path

from capstone import x86

sys.path.insert(0, str(Path(__file__).parent))
from pe import ROOT, build_dir, load as load_image  # noqa: E402

GPR = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"]
SAVED = ("ebx", "esi", "edi", "ebp")
_R = "|".join(GPR)
# Assignments to a register local in lifted C: `r = `, `r += ` (any compound), `r++`/`r--`, or `&r` passed to a helper.
WRITE_RE = re.compile(rf"\b({_R})\s*(?:[-+*/%&|^]|<<|>>)?=(?!=)|\b({_R})\s*(?:\+\+|--)|(?:\+\+|--)\s*({_R})\b|[(,]\s*&\s*({_R})\b")
BOTTOM = "never returns"  # summary of a function no return has been proven reachable for (yet)


def writes(line):
    """GPRs a lifted C line may assign (over-approximation)."""
    return {g for m in WRITE_RE.finditer(line) for g in m.groups() if g}


# Imports bound to another recompiled module (main.c's DLLS; rt_bind_imports points the IAT at its exports).
RECOMPILED_DLLS = {"msvcp120.dll": "msvcp120"}
HOST_RE = re.compile(r'^\s*HOST_CDECL\((\w+),\s*(\w+)\)|^\s*HOST_STDCALL\((\w+),\s*(\w+),\s*(\d+)\)'
                     r'|^\s*HOST\((\w+),\s*\w+,\s*"([^"]+)",\s*(\d+)\)', re.M)


def host_imports():
    """{"dll!name": argbytes} for every HOST declaration (runtime/host.h) in the runtime and generated bridges. A host
    function returns with esp popping its return address plus argbytes, and never changes ebx/esi/edi/ebp (call_guest
    restores them around guest callbacks). Names declared twice with different argbytes are left out."""
    out, bad = {}, set()
    srcs = [p for p in (ROOT / "runtime").glob("*.c") if not p.stem.endswith("_test")]
    srcs += sorted((ROOT / "build/gen_all").glob("*_gen.c"))
    for p in srcs:
        for m in HOST_RE.finditer(p.read_text()):
            if m.group(1):
                dll, name, n = m.group(1), m.group(2), 0
            elif m.group(3):
                dll, name, n = m.group(3), m.group(4), int(m.group(5))
            else:
                dll, name, n = m.group(6), m.group(7), int(m.group(8))
            key = f"{dll.lower()}.dll!{name}"
            if out.get(key, n) != n:
                bad.add(key)
            out[key] = n
    return {k: v for k, v in out.items() if k not in bad}


def import_key(dll, name):
    return f"{dll.lower()}!{name}"


def extern_summaries(module):
    """Summaries for the module's imports, keyed "dll!name" (what the lifter puts in call ops for IAT calls)."""
    img = load_image(module)
    host = host_imports()
    out = {}
    for dll, name in set(img.imports.values()):
        key = import_key(dll, name)
        other = RECOMPILED_DLLS.get(dll.lower())
        if other and other != module:
            sums = load(other) or {}
            addrs = [a for a, names in load_image(other).exports.items() if name in names]
            out[key] = sums.get(addrs[0], UNKNOWN) if len(addrs) == 1 else UNKNOWN
        elif key in host:
            out[key] = (frozenset(SAVED), host[key])
    return out


def cache_key(module, extern):
    """Summaries depend on the lifter, this file, the discovered code and the import summaries."""
    h = hashlib.sha256()
    for p in (Path(__file__), Path(__file__).with_name("lift.py")):
        h.update(p.read_bytes())
    h.update(str((build_dir(module) / "discover.pkl").stat().st_mtime_ns).encode())
    h.update(repr(sorted((k, sorted(v[0]), v[1]) for k, v in extern.items())).encode())
    h.update(VCALL_MODE.encode())
    return h.hexdigest()


def cache_path(module):
    return build_dir(module) / "regsum.pkl"


def load(module):
    """Cached summaries for `module` (imports included), or None if missing or stale (build_all.py writes them)."""
    p = cache_path(module)
    if not p.exists():
        return None
    d = pickle.load(open(p, "rb"))
    return d["summaries"] if d.get("key") == cache_key(module, extern_summaries(module)) else None


def save(module, summaries, extern):
    pickle.dump({"key": cache_key(module, extern), "summaries": summaries}, open(cache_path(module), "wb"))


# ---- facts: per-instruction abstract ops, extracted while lifting ----------------------------------------------------
def insn_ops(lifter, i, lines, ends_with_tail=None):
    """Abstract ops for instruction `i`, whose lifted C is `lines` (strings; sync markers excluded)."""
    m = i.mnemonic
    ops = i.operands
    rn = lifter.reg_name
    w = set()
    for ln in lines:
        w |= writes(ln)

    def reg32(o):
        return o.type == x86.X86_OP_REG and rn(o.reg) in GPR

    def target(op):
        """Call/jump target key: a function address, "dll!name" for an IAT slot, or None (unknown)."""
        if op.type == x86.X86_OP_IMM:
            return op.imm & 0xffffffff
        mm = op.mem if op.type == x86.X86_OP_MEM else None
        if mm and not mm.base and not mm.index and mm.segment in (0, x86.X86_REG_DS):
            imp = lifter.p.img.imports.get(mm.disp & 0xffffffff)
            if imp:
                return import_key(*imp)
        return None

    out = []
    if m in ("ret", "retn") or m.startswith("ret"):
        out.append(("ret", ops[0].imm if ops else 0))
    elif m == "call":
        out.append(("call", target(ops[0])))
        if lifter.p.is_noreturn_call(i.address):
            out.append(("stop",))
    elif m == "jmp" and i.address not in lifter.p.jump_tables:
        op = ops[0]
        if op.type == x86.X86_OP_IMM:
            t = op.imm & 0xffffffff
            if lifter.p.is_func(t) and t != lifter.start:
                out.append(("tail", t))
        else:
            out.append(("tail", target(ops[0])))
    elif m in ("int3", "ud2", "hlt", "int"):
        out.append(("stop",))
    elif m == "push" and reg32(ops[0]) and w <= {"esp"}:
        out.append(("push", rn(ops[0].reg)))
    elif m == "push" and w <= {"esp"}:
        out.append(("push", None))
    elif m == "pop" and reg32(ops[0]):
        out.append(("pop", rn(ops[0].reg)))
    elif m == "leave":
        out += [("mov", "esp", "ebp"), ("pop", "ebp")]
    elif m == "mov" and reg32(ops[0]) and reg32(ops[1]):
        out.append(("mov", rn(ops[0].reg), rn(ops[1].reg)))
    elif m in ("add", "sub") and reg32(ops[0]) and rn(ops[0].reg) == "esp" and ops[1].type == x86.X86_OP_IMM:
        k = ops[1].imm if m == "add" else -ops[1].imm
        out.append(("addesp", ((k + 0x80000000) & 0xffffffff) - 0x80000000))
    elif m == "lea" and reg32(ops[0]) and not ops[1].mem.index and ops[1].mem.segment in (0, x86.X86_REG_DS, x86.X86_REG_SS):
        b = rn(ops[1].mem.base) if ops[1].mem.base else None
        disp = ((ops[1].mem.disp + 0x80000000) & 0xffffffff) - 0x80000000
        out.append(("lea", rn(ops[0].reg), b, disp) if b in GPR else ("kill", frozenset({rn(ops[0].reg)})))
    else:
        # Generic: a memory operand at a known frame offset may be written (reads too; killing a slot is always
        # safe), then the registers the C assigns become unknown.
        for o in ops:
            if o.type == x86.X86_OP_MEM:
                mm = o.mem
                if mm.segment != x86.X86_REG_FS and mm.base and not mm.index and rn(mm.base) in GPR:
                    disp = ((mm.disp + 0x80000000) & 0xffffffff) - 0x80000000
                    out.append(("stw", rn(mm.base), disp, max(o.size, 4) if o.size else 16))
        if w:
            out.append(("kill", frozenset(w)))
    if ends_with_tail is not None:
        out.append(("tail", ends_with_tail))
    return tuple(out)


# ---- the abstract interpretation ------------------------------------------------------------------------------------
# State: (regs, slots). regs maps a GPR to ("E", r) = r's entry value, ("S", k) = entry esp + k, or None = unknown;
# slots maps a stack offset (from entry esp) to the value a push left there.
LOST = ("C",)  # esp is unknown only because calls popped unknown amounts: required() may recover it
RA = ("RA",)  # the return address, in slot 0 at entry


def _entry():
    regs = {r: ("E", r) for r in GPR}
    regs["esp"] = ("S", 0)
    return (regs, {0: RA})


def _join(a, b):
    if a is None:
        return b
    if b is None:
        return a
    ra, sa = a
    rb, sb = b
    regs = {r: (ra[r] if ra[r] == rb[r] else None) for r in GPR}
    if regs["esp"] is None and LOST in (ra["esp"], rb["esp"]) and None not in (ra["esp"], rb["esp"]):
        regs["esp"] = LOST  # known on one path, lost to calls on the other
    slots = {k: v for k, v in sa.items() if sb.get(k) == v}
    return (regs, slots)


def _kill_range(slots, lo, size):
    for k in [k for k in slots if k < lo + size and lo < k + 4]:
        del slots[k]


def _combine(s, t):
    """Join two summaries (BOTTOM is the identity)."""
    if s == BOTTOM:
        return t
    if t == BOTTOM:
        return s
    return (s[0] & t[0], s[1] if s[1] == t[1] else None)


UNKNOWN = (frozenset(), None)
# What an indirect call (vcall, function pointer; not an IAT slot) preserves. MINA4MAC_VCALL=abi (default): ebx/esi/edi/
# ebp, by the calling convention. MSVC only gives a function a custom (LTCG) convention when it sees all its call
# sites, so a function whose address is taken follows cdecl/stdcall/thiscall, and so do host thunks and the recompiled
# msvcp120 exports guest_call can reach (the EH runtime that calls funclets with a borrowed ebp is host code). The
# whole-program proof doesn't go through (regsum can't prove it for ~22% of address-taken functions, mostly through
# CRT helpers it can't see into), so this is an assumption like the ones above; `build_all.py --sync check` verifies
# it at run time. MINA4MAC_VCALL=unknown: indirect calls preserve nothing (the PLAN3 behaviour).
VCALL_MODE = os.environ.get("MINA4MAC_VCALL", "abi")
if VCALL_MODE not in ("abi", "unknown"):
    raise ValueError(f"MINA4MAC_VCALL={VCALL_MODE}")
INDIRECT = (frozenset(SAVED), None) if VCALL_MODE == "abi" else UNKNOWN


ANY = "any"  # required(): no requirement (no return is reachable)


def _summary(summaries, key):
    return summaries.get(key, UNKNOWN) if key is not None else UNKNOWN


def required(insns, summaries):
    """{insn: the esp offset it must start at for a later `ret` to find the return address}, by walking back from the
    returns through instructions that move esp by a known amount. Where esp is lost going forward (a call that pops an
    unknown number of bytes, like a vcall), this recovers it: if esp were anything else there, the original program
    couldn't return. None where unknown or inconsistent."""
    step = {}
    for a, (ops, _) in insns.items():
        d, t = 0, ("delta",)
        for op in ops:
            kind = op[0]
            if kind == "push":
                d -= 4
            elif kind == "pop":
                d += 4
                if op[1] == "esp":
                    t = ("break",)
                    break
            elif kind == "addesp":
                d += op[1]
            elif kind == "lea" and op[1] == "esp":
                if op[2] != "esp":
                    t = ("break",)
                    break
                d += op[3]
            elif (kind == "mov" and op[1] == "esp") or (kind == "kill" and "esp" in op[1]):
                t = ("break",)
                break
            elif kind == "call":
                s = _summary(summaries, op[1])
                if s == BOTTOM:
                    t = ("stop",)
                    break
                if s[1] is None:
                    t = ("break",)
                    break
                d += s[1]
            elif kind in ("ret", "tail"):
                t = ("term", -d)
                break
            elif kind == "stop":
                t = ("stop",)
                break
        step[a] = t if t[0] != "delta" else ("delta", d)
    req = {a: ANY for a in insns}
    preds = {}
    for a, (_, succs) in insns.items():
        for b in succs:
            preds.setdefault(b, []).append(a)
    work = list(insns)
    while work:
        a = work.pop()
        t = step[a]
        if t[0] == "term":
            v = t[1]
        elif t[0] == "stop":
            v = ANY
        elif t[0] == "break":
            v = None
        else:
            v = ANY
            for b in insns[a][1]:
                r = req.get(b, None)
                if r == ANY:
                    continue
                r = None if r is None else r - t[1]
                if v == ANY:
                    v = r
                elif v != r:
                    v = None
                if v is None:
                    break
        if v != req[a]:
            req[a] = v
            work.extend(preds.get(a, ()))
    return req


def analyze(fn, summaries, limit=200):
    """Summary of one function from its facts {"entry": a, "insns": {a: (ops, succs)}} and callee summaries."""
    insns = fn["insns"]
    req = required(insns, summaries)
    state_in = {fn["entry"]: _entry()}
    work = [fn["entry"]]
    visits = {}
    result = BOTTOM
    while work:
        a = work.pop()
        visits[a] = visits.get(a, 0) + 1
        if visits[a] > limit:
            return UNKNOWN
        regs, slots = state_in[a]
        regs, slots = dict(regs), dict(slots)
        if regs["esp"] == LOST and isinstance(req[a], int):
            regs["esp"] = ("S", req[a])
        ops, succs = insns[a]
        live = True
        for op in ops:
            kind = op[0]
            esp = regs["esp"]
            if esp is not None and esp[0] != "S":
                esp = None
                if regs["esp"] != LOST:  # esp holding some other register's entry value: not a frame
                    regs["esp"] = None
            if kind == "push":
                if esp is not None:
                    k = esp[1] - 4
                    _kill_range(slots, k, 4)
                    v = regs[op[1]] if op[1] else None
                    if op[1] == "esp":
                        v = esp
                    if v is not None:
                        slots[k] = v
                    regs["esp"] = ("S", k)
            elif kind == "pop":
                v = slots.get(esp[1]) if esp is not None else None
                if esp is not None:
                    regs["esp"] = ("S", esp[1] + 4)
                if op[1] is not None:
                    regs[op[1]] = v
            elif kind == "mov":
                regs[op[1]] = regs[op[2]]
            elif kind == "addesp":
                if esp is not None:
                    regs["esp"] = ("S", esp[1] + op[1])
            elif kind == "lea":
                _, d, b, disp = op
                v = regs[b]
                if not (d == b == "esp" and v == LOST):
                    regs[d] = ("S", v[1] + disp) if v is not None and v[0] == "S" else None
            elif kind == "kill":
                for r in op[1]:
                    regs[r] = None
            elif kind == "stw":
                _, b, disp, size = op
                v = regs[b]
                if v is not None and v[0] == "S":
                    _kill_range(slots, v[1] + disp, size)
            elif kind == "call":
                s = summaries.get(op[1], UNKNOWN) if op[1] is not None else INDIRECT
                if s == BOTTOM:
                    live = False
                    break
                pres, n = s
                if esp is not None:
                    _kill_range(slots, esp[1] - 4, 4)
                    if n is not None:
                        _kill_range(slots, esp[1], n)
                # Only a call whose target isn't known (a vcall) loses esp recoverably: a known callee whose pops the
                # analysis couldn't prove may be a stack-moving helper like __SEH_prolog4.
                if n is None:
                    regs["esp"] = LOST if op[1] is None and (esp is not None or regs["esp"] == LOST) else None
                elif esp is not None:
                    regs["esp"] = ("S", esp[1] + n)
                for r in ("eax", "ecx", "edx"):
                    regs[r] = None
                for r in SAVED:
                    if r not in pres:
                        regs[r] = None
            elif kind in ("ret", "tail"):
                kept = frozenset(r for r in SAVED if regs[r] == ("E", r))
                ok = esp == ("S", 0) and slots.get(0) == RA  # returning through the original return address
                if kind == "ret":
                    s = (kept, op[1] if ok else None)
                elif op[1] is None:
                    s = (kept & INDIRECT[0], None)
                else:
                    t = summaries.get(op[1], UNKNOWN)
                    s = BOTTOM if t == BOTTOM else (kept & t[0], t[1] if ok else None)
                result = _combine(result, s)
                live = False
                break
            elif kind == "stop":
                live = False
                break
        if not live:
            continue
        st = (regs, slots)
        for b in succs:
            old = state_in.get(b)
            new = _join(old, st) if old is not None else st
            if old is None or new != old:
                state_in[b] = new
                work.append(b)
    return result


def solve(facts, extern):
    """Summaries for all functions: {addr: (preserved frozenset, popped bytes or None) or BOTTOM}, plus `extern` (the
    imports). Callees first (Tarjan SCCs), iterating each recursive SCC to a fixpoint from BOTTOM."""
    callees = {a: {op[1] for ops, _ in f["insns"].values() for op in ops if op[0] in ("call", "tail") and op[1] in facts}
               for a, f in facts.items()}
    summaries = dict(extern)
    for scc in _sccs(facts, callees):
        for a in scc:
            summaries[a] = BOTTOM
        changed = True
        while changed:
            changed = False
            for a in scc:
                s = analyze(facts[a], summaries)
                if s != summaries[a]:
                    summaries[a] = s
                    changed = True
            if len(scc) == 1 and scc[0] not in callees[scc[0]]:
                break
    return summaries


def _sccs(nodes, edges):
    """Tarjan's SCCs (iterative), in reverse topological order: callees before callers."""
    index, low, on, stack, out = {}, {}, set(), [], []
    n = 0
    for root in nodes:
        if root in index:
            continue
        it = [(root, iter(edges[root]))]
        index[root] = low[root] = n
        n += 1
        stack.append(root)
        on.add(root)
        while it:
            v, children = it[-1]
            for w in children:
                if w not in index:
                    index[w] = low[w] = n
                    n += 1
                    stack.append(w)
                    on.add(w)
                    it.append((w, iter(edges[w])))
                    break
                if w in on:
                    low[v] = min(low[v], index[w])
            else:
                it.pop()
                if it:
                    low[it[-1][0]] = min(low[it[-1][0]], low[v])
                if low[v] == index[v]:
                    scc = []
                    while True:
                        w = stack.pop()
                        on.discard(w)
                        scc.append(w)
                        if w == v:
                            break
                    out.append(scc)
    return out


def stats(summaries):
    from collections import Counter
    c = Counter()
    for a, s in summaries.items():
        if isinstance(a, str):
            continue
        if s == BOTTOM:
            c["never returns"] += 1
            continue
        c[f"preserves {len(s[0])}/4"] += 1
        c["pops known" if s[1] is not None else "pops unknown"] += 1
    return c


if __name__ == "__main__":
    import build_all
    module = sys.argv[1] if len(sys.argv) > 1 else "noita"
    s = build_all.regsum_for(module)
    for k, v in sorted(stats(s).items()):
        print(f"{k:16} {v:,}")
