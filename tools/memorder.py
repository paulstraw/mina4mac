"""Memory-ordering audit: find the guest functions whose plain loads/stores take part in lock-free
synchronisation, so the lifter can give them x86 (TSO) ordering on ARM64.

x86 keeps loads in order, stores in order, and stores after earlier loads; ARM64 and the C compiler
keep none of that for plain accesses. Locked instructions are already seq_cst host atomics (cpu.h),
so what can break is the plain side of a lock-free protocol: a flag or counter polled with `mov`,
or a lock released with `mov [lock], 0`. MSVC 2013 compiles std::atomic loads (any order) and
release stores to plain `mov`, and volatile accesses too, so they can't be told apart from ordinary
code; instead we find them by the patterns around them:

  locked  a function with a locked instruction or `xchg mem` (spinlocks, atomic<T> stores, refcounts,
          job counters): all of its instructions, since its plain accesses are the other half of
          the protocol (e.g. an unlock with `mov`).
  yield   a loop that calls a yield/sleep import, directly or through a register: a spin-wait (e.g.
          the job wait at 0x726a51, `while (job->pending > 0) _Thrd_yield();`). The loop's
          instructions; its polled load needs acquire.
  spin    a loop with no stores, no calls and no register carried between iterations that loads a
          non-stack address. Every iteration is the same, so only another thread can end it; clang
          hoists the plain load and compiles it to `b .` (0x84aba0 did). The loop's instructions.
  global  a load or store of a global that some locked instruction also operates on.

The lifter gives these instructions acquire loads and release stores for their non-stack integer
accesses (`Program.is_ordered`, `FnLifter.order`, `rd*_acq`/`wr*_rel` in cpu.h), which on ARM64
(LDAPR/STLR, RCpc) allows only TSO's reordering: a store followed by a load of another address.
x87/SSE memory operands and string ops stay plain.

  uv run tools/memorder.py [module] [-v]   # report; writes build/<module>/memorder.pkl
"""
import pickle
import re
import sys
from collections import defaultdict
from pathlib import Path

from capstone import x86

sys.path.insert(0, str(Path(__file__).parent))
from pe import build_dir  # noqa: E402

YIELD_IMPORTS = {"_Thrd_yield", "?_Yield@_Context@details@Concurrency@@SAXXZ", "__crtSleep", "Sleep",
                 "SwitchToThread", "_Thrd_sleep", "?_SpinOnce@?$_SpinWait@$00@details@Concurrency@@QAE_NXZ",
                 "?_UnderlyingYield@details@Concurrency@@YAXXZ"}
MEM = re.compile(r"\[([^\]]*)\]")


def is_locked(m, o):
    return m.startswith("lock ") or (m == "xchg" and "ptr" in o)


def writes_memory(m, o):
    """Conservative: does this instruction (as text) possibly store to memory?"""
    if m in ("push", "call") or m.startswith(("stos", "movs", "rep", "lock")):
        return True
    if "ptr" not in o:
        return False
    if m in ("cmp", "test", "push") or m.startswith(("j", "cvt", "ucomi", "comi")):
        return False
    return o.split(",")[0].find("ptr") >= 0  # memory destination


def successors(prog, a, bset):
    s, m, o = prog.insns[a]
    out = []
    if m == "jmp":
        if a in prog.jump_tables:
            out = list(prog.jump_tables[a][1])
        elif o.startswith("0x"):
            out = [int(o, 16)]
    elif m.startswith("ret") or m in ("int3", "ud2", "hlt") or prog.is_noreturn_call(a):
        out = []
    else:
        out = [a + s]
        if (m.startswith("j") or m in ("loop", "jecxz")) and o.startswith("0x"):
            out.append(int(o, 16))
    return [x for x in out if x in bset]


def loops(prog, body):
    """Natural-loop-like cycles: for every back edge a -> t (t <= a), the instructions on some path
    t -> ... -> a. Yields (header, sorted instruction list)."""
    bset = set(body)
    succ = {a: successors(prog, a, bset) for a in body}
    pred = defaultdict(list)
    for a, ss in succ.items():
        for t in ss:
            pred[t].append(a)
    for a in body:
        for t in succ[a]:
            if t > a:
                continue
            fwd, work = {t}, [t]
            while work:
                for y in succ[work.pop()]:
                    if y not in fwd:
                        fwd.add(y); work.append(y)
            if a not in fwd:
                continue
            back, work = {a, t}, [a]
            while work:
                for y in pred[work.pop()]:
                    if y not in back and y in fwd:
                        back.add(y); work.append(y)
            yield t, sorted(back)


def analyse(prog):
    ins = prog.insns
    imports = prog.img.imports
    yield_ops = {f"dword ptr [{s:#x}]" for s, (_, n) in imports.items() if n in YIELD_IMPORTS}
    starts = sorted(prog.all_starts | prog.direct_calls)
    reasons = defaultdict(set)
    ordered = set()
    locked_globals = set()
    addrs = sorted(ins)
    index = {a: k for k, a in enumerate(addrs)}
    for a, (s, m, o) in ins.items():
        if not is_locked(m, o):
            continue
        mm = MEM.search(o)
        if mm and re.fullmatch(r"0x[0-9a-f]+", mm.group(1)):
            locked_globals.add(int(mm.group(1), 16))
        elif mm and re.fullmatch(r"e[a-z]{2}", mm.group(1)):
            # `mov ecx, <global>; xchg [ecx], eax` (atomic<T>::store on a global)
            for b in addrs[max(0, index[a] - 6):index[a]][::-1]:
                _, bm, bo = ins[b]
                dst = bo.split(", ")[0]
                if dst == mm.group(1):
                    g = re.fullmatch(r"(?:mov \w+, |lea \w+, (?:\w+ ptr )?\[)(0x[0-9a-f]+)\]?", f"{bm} {bo}")
                    if g and int(g.group(1), 16) >= prog.img.base:
                        locked_globals.add(int(g.group(1), 16))
                    break
    for f in starts:
        try:
            body = prog.function_body(f)
        except Exception:
            continue
        for a in body:
            s, m, o = ins[a]
            if is_locked(m, o):
                reasons[f].add("locked")
            mm = MEM.search(o)
            if mm and re.fullmatch(r"0x[0-9a-f]+", mm.group(1)) and int(mm.group(1), 16) in locked_globals:
                reasons[f].add("global")
                ordered.add(a)
        if "locked" in reasons[f]:
            ordered.update(body)
        # Yield imports are also called through a register loaded from the IAT slot.
        yield_calls = yield_ops | {ins[a][2].split(",")[0] for a in body
                                   if ins[a][1] == "mov" and ins[a][2].split(", ")[-1] in yield_ops}
        for t, region in loops(prog, body):
            if any(ins[x][1] == "call" and ins[x][2] in yield_calls for x in region):
                reasons[f].add("yield")
                ordered.update(region)
            elif spin_loop(prog, t, region):
                reasons[f].add("spin")
                ordered.update(region)
    return {f: r for f, r in reasons.items() if r}, ordered


def spin_loop(prog, header, region):
    """No stores, no calls, no register carried from one iteration to the next, and a non-stack
    load: every iteration is identical, so only another thread's store can end it."""
    ins = prog.insns
    if any(writes_memory(ins[x][1], ins[x][2]) for x in region):
        return False
    order = [x for x in region if x >= header] + [x for x in region if x < header]
    written = set()
    load = False
    for x in order:
        i = prog.decode(x)
        r, w = i.regs_access()
        r = {i.reg_name(k) for k in r} - {"eflags"}
        w = {i.reg_name(k) for k in w} - {"eflags"}
        if i.mnemonic.startswith("xor") and len(i.operands) == 2 and i.op_str.split(", ")[0] == i.op_str.split(", ")[-1]:
            r = set()  # xor r, r
        if {full(k) for k in r} - written & {full(k) for k in w_all(prog, region)}:
            return False  # reads a value from the previous iteration
        written |= {full(k) for k in w}
        for op in i.operands:
            if op.type == x86.X86_OP_MEM and not i.mnemonic.startswith("lea"):
                regs = {i.reg_name(k) for k in (op.mem.base, op.mem.index) if k}
                if not regs & {"esp", "ebp"} and op.mem.segment != x86.X86_REG_FS:
                    load = True
    return load


def w_all(prog, region):
    out = set()
    for x in region:
        i = prog.decode(x)
        out |= {i.reg_name(k) for k in i.regs_access()[1]} - {"eflags"}
    return out


def full(r):
    return {"al": "eax", "ah": "eax", "ax": "eax", "bl": "ebx", "bh": "ebx", "bx": "ebx", "cl": "ecx",
            "ch": "ecx", "cx": "ecx", "dl": "edx", "dh": "edx", "dx": "edx", "si": "esi", "di": "edi",
            "bp": "ebp", "sp": "esp"}.get(r, r)


def ordered_insns(prog):
    """(function -> reasons, set of instruction addresses to lift with acquire/release), cached in
    build/<module>/memorder.pkl (keyed by the discover.pkl mtime)."""
    cache = build_dir(prog.module) / "memorder.pkl"
    stamp = (build_dir(prog.module) / "discover.pkl").stat().st_mtime
    if cache.exists():
        d = pickle.load(open(cache, "rb"))
        if d.get("stamp") == stamp and d.get("version") == VERSION:
            return d["reasons"], d["ordered"]
    reasons, ordered = analyse(prog)
    pickle.dump({"stamp": stamp, "version": VERSION, "reasons": reasons, "ordered": ordered}, open(cache, "wb"))
    return reasons, ordered


VERSION = 5


def main():
    from lift import Program
    args = [a for a in sys.argv[1:] if not a.startswith("-")]
    prog = Program(args[0] if args else "noita")
    reasons, ordered = ordered_insns(prog)
    by = defaultdict(list)
    for f, r in reasons.items():
        for k in r:
            by[k].append(f)
    print(f"{prog.module}: {len(reasons)} functions, {len(ordered)} instructions ordered; " +
          ", ".join(f"{k} {len(v)}" for k, v in sorted(by.items())))
    if "-v" in sys.argv:
        for f in sorted(reasons):
            print(f"  {f:#x} {','.join(sorted(reasons[f]))}")


if __name__ == "__main__":
    main()
