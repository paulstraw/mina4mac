"""Stack slots in C locals (PLAN4 Phase 16): which of a function's own stack slots its lifted code may keep in locals.

A plan names, per function, the instructions whose memory operand is a promoted slot: a fixed offset from a frame
base and a size (1, 2 or 4 bytes), accessed through the lifter's rd/wr. The lifter declares a local per slot, loaded
from the stack, reads the local instead of memory and writes both ("store-through"). Memory stays current, so
everything that reads the frame another way (callees reading pushed arguments, x87/SSE loads, partial reads) still
sees the right bytes. A local can only go stale through a write that bypasses it, so a slot is promoted only if the
analysis sees every write that may reach it:

  - Frame addresses are tracked through registers: "entry esp + k" (base S), or "aligned esp + k" (base A) after an
    `and esp, -N` (one per function; A lies in [S - (N-1), S], so spans compare as intervals).
  - An escape (a frame address stored, pushed, passed in a register to a call, returned, or read by an instruction
    we don't model) taints the frame from that address up: a callee or another thread may write it through the
    pointer, and a pointer to an object reaches only that object, which lies above its address. Slots overlapping
    a taint stay in memory. An escape of a frame address at an unknown offset gets the function no slots.
    Not escapes: /GS cookies (`xor eax, esp`, a value) and EH registration (`mov fs:[0], eax`; guest C++ exceptions
    aren't supported, a throw is fatal, so nothing walks the chain into the frame. Revisit if that changes).
  - A frame access at an unknown offset (through a frame address the analysis lost, with a frame index, a string op
    or an unsized write on the frame, a push while esp is unknown) gets the function no slots ("wild").
  - A known-offset write that bypasses rd/wr (push, x87/SSE stores, locked ops, another slot's write that
    overlaps) pins the bytes it covers.
  - A direct callee that may move esp (pops unknown) or not preserve ebp (like __SEH_prolog4, which sets its caller's
    ebp) leaves esp or ebp an unknown frame address, so later accesses through them are wild.
Reads through any path are safe (memory is current). Writes by callees to their own argument bytes are, by every
x86 calling convention, never read back by the caller, and code never reads below esp, so calls clobbering the area
below the caller's esp don't matter either.

esp after an indirect call: all targets of one call site pop the same number of bytes (the caller's code after it
expects one esp), so a target observed at the site in an indirect-call profile (`site_targets`,
tools/icall_targets.txt) gives it through its summary. Otherwise it's recovered backwards from a `ret` (required()),
or forwards where the path joins one that knows it (compiled code has one esp per instruction).
`MINA4MAC_SLOTS=check` builds verify both the offsets and the locals at run time (lift.py).

Opt-in (`build_all.py --slots on|check`): PLAN4 Phase 16 measured no gain at the coverage this reaches.

  uv run tools/slots.py [module]    statistics over the cached plans (build_all.py --slots, or tools/regsum.py)
  uv run tools/slots.py --targets build/icprof/heavy.tsv ...    write tools/icall_targets.txt from --icprof profiles
"""
import sys
from pathlib import Path

from capstone import x86

sys.path.insert(0, str(Path(__file__).parent))
import regsum  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
SITE_TARGETS = ROOT / "tools/icall_targets.txt"
GPR = regsum.GPR
R8 = {"al": "eax", "cl": "ecx", "dl": "edx", "bl": "ebx", "ah": "eax", "ch": "ecx", "dh": "edx", "bh": "ebx"}
R16 = {"ax": "eax", "cx": "ecx", "dx": "edx", "bx": "ebx", "sp": "esp", "bp": "ebp", "si": "esi", "di": "edi"}
STRING = {op + w for op in ("movs", "stos", "lods", "cmps", "scas") for w in "bwd"}  # without xmm operands (movsd)
READ, WRITE = 1, 2  # capstone's CS_AC_READ, CS_AC_WRITE
F = "F"  # a frame address at an unknown offset
LOST = regsum.LOST  # esp after an indirect call whose pops we don't know (yet)


def _gpr(name):
    return name if name in GPR else R16.get(name) or R8.get(name)


def _s32(v):
    return ((v + 0x80000000) & 0xffffffff) - 0x80000000


def load_site_targets(path=SITE_TARGETS):
    """{indirect call site: a target observed there} from tools/icall_targets.txt ("site target" hex lines)."""
    if not path.exists():
        return {}
    out = {}
    for line in path.read_text().splitlines():
        w = line.split("#")[0].split()
        if len(w) == 2:
            out[int(w[0], 16)] = int(w[1], 16)
    return out


# ---- facts: per-instruction frame facts, extracted while lifting -----------------------------------------------------
def insn_frame(lifter, i):
    """(memory accesses, registers read as values, modelled register arithmetic) of instruction `i`, after lifting.
    An access is (base, index, disp, size, via, write): via "rw" if the lifted code accesses memory only through
    rd/wr (so a plan can route it through a slot local), "s" for string ops, else "x"."""
    m = i.mnemonic.split()[-1]
    rn = lifter.reg_name
    ops = i.operands
    via = "rw" if i.address in lifter.rw_insns and i.address not in lifter.x_insns else "x"
    if m in STRING and "xmm" not in i.op_str:
        via = "s"
    mems, reads, arith = [], set(), None
    for o in ops:
        if o.type == x86.X86_OP_MEM:
            mm = o.mem
            if m == "lea":  # without an index regsum models it ("lea"); with one, base and index are used as values
                if mm.index:
                    reads |= {r for r in (_gpr(rn(mm.base)) if mm.base else None, _gpr(rn(mm.index))) if r}
                continue
            if mm.segment == x86.X86_REG_FS:
                src = ops[1] if len(ops) == 2 else None
                if m == "mov" and not mm.base and not mm.index and not mm.disp and src.type == x86.X86_OP_REG:
                    arith = (_gpr(rn(src.reg)), "ehreg")
                continue
            mems.append((_gpr(rn(mm.base)) if mm.base else None, _gpr(rn(mm.index)) if mm.index else None,
                         _s32(mm.disp), o.size, via, bool(o.access & WRITE) or not o.access))
        elif o.type == x86.X86_OP_REG and o.access & READ:
            r = _gpr(rn(o.reg))
            if r:
                reads.add(r)
    r0 = rn(ops[0].reg) if ops and ops[0].type == x86.X86_OP_REG else None
    imm1 = _s32(ops[1].imm) if len(ops) == 2 and ops[1].type == x86.X86_OP_IMM else None
    if arith:
        pass
    elif m in ("add", "sub") and r0 in GPR and imm1 is not None:
        arith = (r0, imm1 if m == "add" else -imm1)
    elif m in ("inc", "dec") and r0 in GPR:
        arith = (r0, 1 if m == "inc" else -1)
    elif m == "and" and r0 == "esp" and imm1 in (-8, -16, -32, -64):
        arith = ("esp", "align", -imm1)
    elif m == "xor" and r0 in GPR and r0 not in ("esp", "ebp") and len(ops) == 2 and ops[1].type == x86.X86_OP_REG \
            and rn(ops[1].reg) in ("esp", "ebp"):
        arith = (r0, "cookie")  # /GS: the security cookie xor esp (or ebp), a value, not an address
    if m in ("pushal", "pushad", "pusha"):
        reads.add("esp")
    return (tuple(mems), frozenset(reads), arith)


# ---- the analysis ----------------------------------------------------------------------------------------------------
def _known(v):
    """A frame address at a known offset: ("S", k) = entry esp + k, or ("A", k) = the aligned esp + k."""
    return v is not None and v != F and v != LOST


def _join(a, b):
    out = {}
    for r in GPR:
        x, y = a[r], b[r]
        if x == y:
            out[r] = x
        elif (x == LOST and _known(y)) or (y == LOST and _known(x)):
            out[r] = x if x != LOST else y  # compiled code has one esp per instruction
        elif x is not None or y is not None:
            out[r] = F
        else:
            out[r] = None
    return out


class Bail(Exception):
    """The function gets no slots (wild access, an escape at an unknown offset, or too many iterations)."""


class Callees:
    """What a call to a function does to its caller's frame, under the calling convention. regsum proves too little to
    rely on (an unproven callee's unproven callee cascades up; a third of the functions have unknown pops), so a
    callee follows the convention unless its own code says otherwise:
      - ebp_mover(t): may return with ebp changed. Frame-building helpers like __SEH_prolog4/__EH_prolog3 point their
        caller's ebp into its frame, __SEH_epilog4 restores the caller's caller's. Structurally: it (or a function it
        tail-calls) writes ebp other than by `mov ebp, esp`, `leave` or a `pop ebp` of an ebp it pushed.
      - pops(t): the bytes it pops besides the return address: regsum's if proven, else the immediate of its `ret`s
        (all equal) unless it moves esp in a way that doesn't restore it (__chkstk, __SEH_prolog4...): then None.
    Imports (host code, msvcp120 exports) follow the convention with regsum's pops."""

    def __init__(self, facts_of, summaries):
        self.facts_of = facts_of  # function address -> its facts, or None (not a lifted function)
        self.summaries = summaries
        self.memo = {}

    def info(self, t, depth=0):
        """(ebp mover, pops or None) of function `t`."""
        if t in self.memo:
            return self.memo[t]
        s = self.summaries.get(t)
        proven = s is not None and s != regsum.BOTTOM
        self.memo[t] = (True, None)  # a tail-call cycle: assume the worst
        fn = self.facts_of(t)
        mover, rets, esp_bad = False, set(), False
        if fn is not None:
            pushes = pops = 0
            frame = False  # mov ebp, esp: the function can restore esp from ebp
            for ops, _ in fn["insns"].values():
                for op in ops:
                    k = op[0]
                    if k == "push" and op[1] == "ebp":
                        pushes += 1
                    elif k == "pop" and op[1] == "ebp":
                        pops += 1
                    elif k == "pop" and op[1] == "esp":
                        esp_bad = True
                    elif k == "mov" and op[1] == "ebp":
                        if op[2] == "esp":
                            frame = True
                        else:
                            mover = True
                    elif k == "mov" and op[1] == "esp" and op[2] != "ebp":
                        esp_bad = True
                    elif k == "lea" and op[1] == "ebp":
                        mover = True
                    elif k == "lea" and op[1] == "esp" and op[2] not in ("esp", "ebp"):
                        esp_bad = True
                    elif k == "kill" and "ebp" in op[1]:
                        mover = True
                    elif k == "kill" and "esp" in op[1]:
                        esp_bad = esp_bad or "escape"  # and esp, -16 / sub esp, eax: fine if restored from ebp
                    elif k == "ret":
                        rets.add(op[1])
                    elif k == "tail":
                        if op[1] is None or isinstance(op[1], str):
                            rets.add(None)
                        else:
                            m2, n2 = self.info(op[1], depth + 1) if depth < 8 else (True, None)
                            mover = mover or m2
                            rets.add(n2)
            mover = mover or bool(pops and not pushes)
            if esp_bad == "escape" and frame:
                esp_bad = False
        if proven and "ebp" in s[0]:
            mover = False
        n = s[1] if proven else None
        if n is None and fn is not None and not esp_bad and not mover and len(rets) == 1:
            n = next(iter(rets))
        self.memo[t] = (mover, n)
        return self.memo[t]

    def ebp_mover(self, t):
        return isinstance(t, int) and self.info(t)[0]

    def pops(self, t):
        if not isinstance(t, int):
            s = self.summaries.get(t)
            return s[1] if s is not None and s != regsum.BOTTOM else None
        return self.info(t)[1]


class Fn:
    """One function's analysis state: its facts, callee summaries and what the walk found."""

    def __init__(self, fn, summaries, site_targets, callees):
        self.insns, self.frame = fn["insns"], fn["frame"]
        self.summaries = summaries
        self.site_targets = site_targets or {}
        self.callees = callees
        self.align = None  # (address, entry-esp offset before it, N) of the `and esp, -N`
        self.taints = []  # entry-esp offsets from which the frame may be written through escaped pointers

    def call_summary(self, a, target):
        """(preserved registers, popped bytes or None) for a call at `a` to `target` (None = indirect: its site's
        observed target stands in for it). BOTTOM if it never returns. ebx/esi/edi are the calling convention's
        (they only matter here while they hold frame addresses, which a call taints), ebp too unless the callee is an
        ebp mover (Callees)."""
        t = self.site_targets.get(a) if target is None else target
        if target is not None and self.summaries.get(target) == regsum.BOTTOM:
            return regsum.BOTTOM
        n = self.callees.pops(t) if t is not None else None
        if target is not None and self.callees.ebp_mover(target):
            return (frozenset(("ebx", "esi", "edi")), n)
        return (frozenset(regsum.SAVED), n)

    def lo(self, v):
        """The lowest entry-esp offset a known frame address may be."""
        return v[1] if v[0] == "S" else self.align[1] + v[1] - (self.align[2] - 1)

    def escape(self, v, why):
        if not _known(v):
            raise Bail(f"escape {why}")
        self.taints.append(self.lo(v))


def analyze(fn, summaries, site_targets=None, callees=None, limit=200):
    """Plan for one function from its facts ({"entry", "insns": {a: (ops, succs)}, "frame": {a: insn_frame}}):
    {insn address: slot}, slot = (base, offset, size), plus plan["loads"] = the routed reads that load the slot from
    memory, and plan["align"] = (address, entry-esp offset before it, N) if a slot is in the aligned frame. Raises
    Bail with the reason if the function gets none."""
    st = Fn(fn, summaries, site_targets, callees or Callees(lambda t: None, summaries))
    req = required(st)
    entry = {r: None for r in GPR}
    entry["esp"] = ("S", 0)
    state_in = {fn["entry"]: entry}
    work = [fn["entry"]]
    visits = {}
    at = {}  # insn -> the state it starts in (the last visit's, which is the fixpoint's)
    while work:
        a = work.pop()
        visits[a] = visits.get(a, 0) + 1
        if visits[a] > limit:
            raise Bail("limit")
        regs = dict(state_in[a])
        if regs["esp"] == LOST and req.get(a) is not None:
            regs["esp"] = ("S", req[a])
        at[a] = dict(regs)
        if not _step(st, regs, a):
            continue
        for b in st.insns[a][1]:
            old = state_in.get(b)
            new = _join(old, regs) if old is not None else regs
            if old is None or new != old:
                state_in[b] = new
                work.append(b)
    return _plan(st, at, fn["entry"])


def required(st):
    """{insn: the entry-esp offset it must start at for a later `ret` to find the return address}, walking back from
    the returns through instructions that move esp by known amounts (regsum.required(), but a successor with no
    known requirement doesn't make its predecessors' unknown: any path to a `ret` that knows it is enough)."""
    step = {}
    for a, (ops, _) in st.insns.items():
        d, t = 0, None
        for op in ops:
            kind = op[0]
            if kind == "push":
                d -= 4
            elif kind == "pop":
                d += 4
                if op[1] == "esp":
                    t = "break"
            elif kind == "addesp":
                d += op[1]
            elif kind == "lea" and op[1] == "esp":
                if op[2] != "esp":
                    t = "break"
                d += op[3]
            elif (kind == "mov" and op[1] == "esp") or (kind == "kill" and "esp" in op[1]) or kind == "stop":
                t = "break"
            elif kind == "call":
                s = st.call_summary(a, op[1])
                if s == regsum.BOTTOM or s[1] is None:
                    t = "break"
                else:
                    d += s[1]
            elif kind in ("ret", "tail"):
                t = ("term", -d)
            if t is not None:
                break
        step[a] = t if t is not None else ("delta", d)
    req = {}
    preds = {}
    for a, (_, succs) in st.insns.items():
        for b in succs:
            preds.setdefault(b, []).append(a)
    bad = set()
    work = list(st.insns)
    while work:
        a = work.pop()
        t = step[a]
        if t == "break" or a in bad:
            continue
        if t[0] == "term":
            v = t[1]
        else:
            vs = {req[b] - t[1] for b in st.insns[a][1] if req.get(b) is not None}
            if len(vs) > 1:
                bad.add(a)
                req.pop(a, None)
                work.extend(preds.get(a, ()))
                continue
            v = vs.pop() if vs else None
        if v is not None and req.get(a) != v:
            if a in req:  # conflicting requirements: give up on this instruction for good
                bad.add(a)
                del req[a]
            else:
                req[a] = v
            work.extend(preds.get(a, ()))
    return req


def _step(st, regs, a):
    """Apply instruction `a` to `regs` in place; False if control doesn't continue."""
    ops = st.insns[a][0]
    mems, reads, arith = st.frame[a]
    legal = set()  # value reads of frame registers this instruction's modelled ops account for
    for op in ops:
        if op[0] == "mov":
            legal.add(op[2])
        elif op[0] == "addesp":
            legal.add("esp")
    if arith and arith[1] == "cookie":
        legal |= {"esp", "ebp"}
    elif arith:
        legal.add(arith[0])
    escaped = False
    for r in reads - legal:
        if regs[r] is not None:
            st.escape(regs[r], f"{a:#x} {r}")
            escaped = True
    for base, index, disp, size, via, write in mems:
        b = regs[base] if base else None
        if (index and regs[index] is not None) or (b is not None and (index or not _known(b))):
            raise Bail(f"wild {a:#x}")
        if b is not None and (via == "s" or (write and not size)):
            raise Bail(f"wild string/unsized {a:#x}")
        if base == "esp" and any(op[0] == "pop" for op in ops):
            raise Bail(f"wild pop [esp] {a:#x}")  # its address uses esp after the pop
    for op in ops:
        kind = op[0]
        esp = regs["esp"]
        if kind in ("push", "pop") and not _known(esp):
            raise Bail(f"{kind} at unknown esp {a:#x}")
        if kind == "push":
            if op[1] and regs[op[1]] is not None:
                st.escape(regs[op[1]], f"push {a:#x}")
            regs["esp"] = (esp[0], esp[1] - 4)
        elif kind == "pop":
            regs["esp"] = (esp[0], esp[1] + 4)
            if op[1] is not None:
                regs[op[1]] = F if op[1] == "esp" else None
        elif kind == "mov":
            _, d, s = op
            regs[d] = regs[s] if d != "esp" or regs[s] is not None else F  # esp is always a frame address
        elif kind == "addesp":
            regs["esp"] = (esp[0], esp[1] + op[1]) if _known(esp) else F
        elif kind == "lea":
            _, d, b, disp = op
            v = regs[b] if b else None
            if d == b == "esp" and v == LOST:
                continue
            if v is None:
                regs[d] = F if d == "esp" else None
            else:
                regs[d] = (v[0], v[1] + disp) if _known(v) else F
        elif kind == "kill":
            for r in op[1]:
                v = regs[r]
                if arith and arith[0] == r and arith[1] == "align":
                    if not (_known(v) and v[0] == "S") or (st.align or (a, v[1], arith[2])) != (a, v[1], arith[2]):
                        raise Bail(f"wild align {a:#x}")
                    st.align = (a, v[1], arith[2])
                    regs[r] = ("A", 0)
                elif arith and arith[0] == r and arith[1] in ("cookie", "ehreg"):
                    regs[r] = None
                elif arith and arith[0] == r and _known(v):
                    regs[r] = (v[0], v[1] + arith[1])
                elif r == "esp" or escaped:
                    regs[r] = F  # esp moved some other way, or a value computed from a frame address
                else:
                    regs[r] = None
        elif kind in ("call", "tail"):
            for r in GPR:
                if r not in ("esp", "ebp") and regs[r] is not None:
                    st.escape(regs[r], f"{kind} {a:#x} {r}")
            if kind == "tail":
                return False
            s = st.call_summary(a, op[1])
            if s == regsum.BOTTOM:
                return False
            pres, n = s
            if n is None:
                # Recoverable (required(), joins) unless the callee may be a frame-building helper like
                # __SEH_prolog4, which also sets ebp (and writes the frame: then the function is wild).
                regs["esp"] = LOST if esp not in (None, F) and "ebp" in pres else F
            else:
                regs["esp"] = (esp[0], esp[1] + n) if _known(esp) else F
            for r in ("eax", "ecx", "edx", "ebx", "esi", "edi"):
                if r not in pres:
                    regs[r] = None
            if "ebp" not in pres:
                regs["ebp"] = F  # a helper like __SEH_prolog4 may point it into our frame
        elif kind == "ret":
            if regs["eax"] is not None:
                st.escape(regs["eax"], f"ret {a:#x}")
            return False
        elif kind == "stop":
            return False
    return True


def _plan(st, at, entry):
    """Promote each slot accessed through rd/wr whose bytes nothing else may write: no pin, taint or other slot with
    writes overlaps it."""
    def span(base, o, size):  # entry-esp interval of everything the bytes may be
        if base == "S":
            return (o, o + size)
        return (st.align[1] + o - (st.align[2] - 1), st.align[1] + o + size)
    acc = {}  # slot -> ([insn], written)
    pins = []
    for a, regs in at.items():
        esp = regs["esp"]
        for op in st.insns[a][0]:
            if op[0] == "push" and _known(esp):
                pins.append(span(esp[0], esp[1] - 4, 4))
                esp = (esp[0], esp[1] - 4)
            elif op[0] == "pop" and _known(esp):
                esp = (esp[0], esp[1] + 4)
        for base, index, disp, size, via, write in st.frame[a][0]:
            v = regs[base] if base else None
            if not _known(v) or index:
                continue  # not the frame (wild frame accesses bailed in _step)
            if via == "rw" and size in (1, 2, 4):
                sl = (v[0], v[1] + disp, size)
                where, w = acc.get(sl, ([], False))
                where.append(a)
                acc[sl] = (where, w or write)
            elif write:
                pins.append(span(v[0], v[1] + disp, size))
    taint = min(st.taints, default=None)
    spans = sorted((span(*sl), sl, w) for sl, (_, w) in acc.items())
    bad = set()
    for k, ((lo, hi), sl, w) in enumerate(spans):
        for (lo2, hi2), sl2, w2 in spans[k + 1:]:
            if lo2 >= hi:
                break
            if w2:
                bad.add(sl)
            if w:
                bad.add(sl2)
    plan = {}
    for sl, (where, _) in acc.items():
        lo, hi = span(*sl)
        if sl in bad or (taint is not None and hi > taint) or any(plo < hi and lo < phi for plo, phi in pins):
            continue
        for a in where:
            plan[a] = sl
    if plan:
        plan["loads"] = _loads(st, at, plan, entry)
    if any(sl[0] == "A" for k, sl in plan.items() if isinstance(k, int)):
        plan["align"] = st.align
    return plan


def _loads(st, at, plan, entry):
    """The routed reads that must load the slot from memory (into its local): those some path reaches without an
    access to the slot since function entry (or the `and esp` that starts the aligned frame). A forward
    must-analysis, so a slot costs nothing on paths that don't use it."""
    align = st.align[0] if st.align else None
    avail_in = {entry: frozenset()}
    work = [entry]
    while work:
        a = work.pop()
        av = avail_in[a]
        if a == align:
            av = frozenset(sl for sl in av if sl[0] != "A")
        if a in plan:
            av = av | {plan[a]}
        for b in st.insns[a][1]:
            if b not in at:
                continue
            old = avail_in.get(b)
            new = av if old is None else old & av
            if new != old:
                avail_in[b] = new
                work.append(b)
    return frozenset(a for a, sl in plan.items() if isinstance(a, int) and sl not in avail_in.get(a, frozenset()))


def solve(facts, summaries, site_targets=None):
    """{function: plan} for the functions that get slots, and {reason: count} for the ones that don't."""
    plans, why = {}, {}
    callees = Callees(facts.get, summaries)
    for a, fn in facts.items():
        if "frame" not in fn:
            continue
        try:
            p = analyze(fn, summaries, site_targets, callees)
        except Bail as e:
            k = str(e).split()[0]
            why[k] = why.get(k, 0) + 1
            continue
        if p:
            plans[a] = p
        else:
            why["none"] = why.get("none", 0) + 1
    return plans, why


def write_site_targets(profiles, path=SITE_TARGETS):
    """One target per indirect call site (its most-called one) from --icprof profiles ("site target calls" lines).
    Import thunks (0xf0000000 and up) are left out: they have no address summary."""
    best = {}
    for p in profiles:
        for line in Path(p).read_text().splitlines():
            w = line.split()
            if len(w) != 3:
                continue
            site, t, n = int(w[0], 16), int(w[1], 16), int(w[2])
            if t < 0xf0000000 and n > best.get(site, (0, 0))[1]:
                best[site] = (t, n)
    path.write_text("# indirect call site -> a target observed there (tools/slots.py --targets; from --icprof runs)\n"
                    + "".join(f"{s:#x} {t:#x}\n" for s, (t, _) in sorted(best.items())))
    print(f"{len(best):,} sites -> {path.relative_to(ROOT)}")


if __name__ == "__main__":
    if sys.argv[1:2] == ["--targets"]:
        write_site_targets(sys.argv[2:])
        sys.exit()
    module = sys.argv[1] if len(sys.argv) > 1 else "noita"
    plans = regsum.load_slots(module)
    if plans is None:
        sys.exit("no fresh slot plans: run tools/build_all.py (or tools/regsum.py) first")
    n = sum(sum(isinstance(k, int) for k in p) for p in plans.values())
    slots = sum(len({v for k, v in p.items() if isinstance(k, int)}) for p in plans.values())
    print(f"functions with slots {len(plans):,}; slots {slots:,}; accesses routed {n:,}")
