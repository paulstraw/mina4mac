"""Static recompiler: x86-32 functions from one module (noita.exe, or a DLL at its chosen base) -> C.

Each guest function becomes `void F_<addr>(CPU *c)`. General-purpose registers and flags live in
C locals for the body of the function and are synced to `c` around calls and at exit, so clang can
keep them in host registers and drop flag computations nobody reads.
"""
import pickle
import re
import sys
from pathlib import Path

import capstone
from capstone import x86

sys.path.insert(0, str(Path(__file__).parent))
from pe import ROOT, build_dir, load  # noqa: E402

GPR = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"]
R8 = {"al": ("eax", 0), "cl": ("ecx", 0), "dl": ("edx", 0), "bl": ("ebx", 0),
      "ah": ("eax", 8), "ch": ("ecx", 8), "dh": ("edx", 8), "bh": ("ebx", 8)}
R16 = {"ax": "eax", "cx": "ecx", "dx": "edx", "bx": "ebx", "sp": "esp", "bp": "ebp", "si": "esi", "di": "edi"}
SREG = {"cs": "SEL_CS", "ds": "SEL_DS", "es": "SEL_DS", "ss": "SEL_DS", "gs": "SEL_DS", "fs": "SEL_FS"}
# Imports that never return. A call through one ends the block, so data placed after it
# (e.g. a jump table) isn't treated as code.
NORETURN_IMPORTS = {
    "?_Xlength_error@std@@YAXPBD@Z", "?_Xout_of_range@std@@YAXPBD@Z", "?_Xbad_alloc@std@@YAXXZ",
    "?_Xinvalid_argument@std@@YAXPBD@Z", "?_Xbad_function_call@std@@YAXXZ",
    "?_Throw_C_error@std@@YAXH@Z", "?_Throw_Cpp_error@std@@YAXH@Z", "?_Throw_future_error@std@@YAXABVerror_code@1@@Z",
    "?_Rethrow_future_exception@std@@YAXVexception_ptr@1@@Z", "_CxxThrowException", "_exit", "exit",
    "_amsg_exit", "?terminate@@YAXXZ", "longjmp",
}
MASK = {1: "0xffu", 2: "0xffffu", 4: "0xffffffffu"}
UT = {1: "uint8_t", 2: "uint16_t", 4: "uint32_t", 8: "uint64_t"}
ST = {1: "int8_t", 2: "int16_t", 4: "int32_t", 8: "int64_t"}
RD = {1: "rd8", 2: "rd16", 4: "rd32", 8: "rd64"}
WR = {1: "wr8", 2: "wr16", 4: "wr32", 8: "wr64"}

CC = {
    "o": "of", "no": "!of", "b": "cf", "ae": "!cf", "e": "zf", "ne": "!zf",
    "be": "(cf|zf)", "a": "!(cf|zf)", "s": "sf", "ns": "!sf", "p": "pf", "np": "!pf",
    "l": "(sf!=of)", "ge": "(sf==of)", "le": "(zf|(sf!=of))", "g": "(!zf&(sf==of))",
}


class Unsupported(Exception):
    pass


class Program:
    """Whole-image knowledge the lifter needs: decoded instructions, function starts, tables."""

    def __init__(self, module="noita"):
        self.module = module
        self.img = load(module)
        d = pickle.load(open(build_dir(module) / "discover.pkl", "rb"))
        self.insns = d["insns"]
        self.jump_tables = d["jump_tables"]
        self.all_starts = set(d["calls"])
        self.direct_calls = set()
        for a, (s, m, o) in self.insns.items():
            if m == "call" and o.startswith("0x"):
                self.direct_calls.add(int(o, 16))
        self.t_lo, self.t_hi = self.img.section(".text")
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        self.md.detail = True
        self.noreturn_ops = {f"dword ptr [{slot:#x}]" for slot, (_, name) in self.img.imports.items()
                             if name in NORETURN_IMPORTS}

    def is_noreturn_call(self, a):
        _, m, o = self.insns[a]
        return m == "call" and o in self.noreturn_ops

    def decode(self, addr):
        return next(self.md.disasm(self.img.read(addr, 16), addr, 1))

    def is_func(self, a):
        return a in self.direct_calls or a in self.all_starts

    def function_body(self, start):
        """Addresses of instructions belonging to the function at `start` (intra-procedural reach)."""
        body = set()
        work = [start]
        while work:
            a = work.pop()
            while a not in body:
                if a not in self.insns:
                    raise Unsupported(f"undecoded {a:#x}")
                body.add(a)
                size, m, o = self.insns[a]
                nxt = a + size
                if m == "jmp":
                    if a in self.jump_tables:
                        work.extend(self.jump_tables[a][1])
                    elif o.startswith("0x"):
                        t = int(o, 16)
                        if not (self.is_func(t) and t != start):
                            work.append(t)
                    break
                if m.startswith("j") or m in ("loop", "jecxz"):
                    work.append(int(o, 16))
                if m in ("ret", "int3", "ud2", "hlt") or m.startswith("ret") or self.is_noreturn_call(a):
                    break
                a = nxt
        return sorted(body)


class FnLifter:
    def __init__(self, prog: Program, start: int):
        self.p = prog
        self.start = start
        self.lines = []
        self.labels = set()
        self.import_slots = set()  # IAT slots called or jumped through

    # ---- operands -------------------------------------------------------------------------
    def reg_name(self, r):
        return self.cur.reg_name(r)

    def addr(self, mem):
        parts = []
        if mem.base:
            parts.append(self.reg_name(mem.base))
        if mem.index:
            idx = self.reg_name(mem.index)
            parts.append(f"{idx}*{mem.scale}" if mem.scale != 1 else idx)
        if mem.disp or not parts:
            parts.append(f"{mem.disp & 0xffffffff:#x}u")
        e = "+".join(parts)
        if mem.segment == x86.X86_REG_FS:
            e = f"c->fs_base+{e}"
        elif mem.segment not in (0, x86.X86_REG_DS, x86.X86_REG_SS, x86.X86_REG_ES):
            raise Unsupported("segment")
        return f"(uint32_t)({e})"

    def rd(self, op, size=None):
        size = size or op.size
        if op.type == x86.X86_OP_REG:
            n = self.reg_name(op.reg)
            if n in GPR:
                return n if size == 4 else f"({UT[size]}){n}"
            if n in R16:
                return f"(uint16_t){R16[n]}"
            if n in R8:
                r, sh = R8[n]
                return f"(uint8_t)({r}>>{sh})" if sh else f"(uint8_t){r}"
            if n in SREG:
                return f"({UT[size]}){SREG[n]}"
            raise Unsupported(f"reg {n}")
        if op.type == x86.X86_OP_IMM:
            return f"({UT[size]}){op.imm & ((1 << (8 * size)) - 1):#x}u"
        if op.type == x86.X86_OP_MEM:
            return f"{RD[size]}({self.addr(op.mem)})"
        raise Unsupported("operand")

    def wr(self, op, val, size=None):
        size = size or op.size
        if op.type == x86.X86_OP_REG:
            n = self.reg_name(op.reg)
            if n in GPR:
                self.emit(f"{n} = (uint32_t)({val});")
            elif n in R16:
                r = R16[n]
                self.emit(f"{r} = ({r} & 0xffff0000u) | (uint16_t)({val});")
            elif n in R8:
                r, sh = R8[n]
                self.emit(f"{r} = ({r} & ~(0xffu<<{sh})) | ((uint32_t)(uint8_t)({val})<<{sh});")
            else:
                raise Unsupported(f"reg {n}")
        elif op.type == x86.X86_OP_MEM:
            self.emit(f"{WR[size]}({self.addr(op.mem)}, ({UT[size]})({val}));")
        else:
            raise Unsupported("write operand")

    def xmm(self, op):
        n = self.reg_name(op.reg) if op.type == x86.X86_OP_REG else None
        if not n or not n.startswith("xmm"):
            raise Unsupported(n)
        return f"c->xmm[{n[3:]}]"

    def emit(self, s):
        self.lines.append("  " + s)

    # ---- flags ----------------------------------------------------------------------------
    def flags_logic(self, r, size):
        bits = 8 * size
        self.emit(f"zf = ({UT[size]}){r} == 0; sf = ((uint32_t){r} >> {bits - 1}) & 1; "
                  f"cf = 0; of = 0; pf = parity8({r});")

    def flags_add(self, a, b, r, size, carry_in="0"):
        bits = 8 * size
        self.emit(f"zf = ({UT[size]}){r} == 0; sf = ((uint32_t){r} >> {bits - 1}) & 1; pf = parity8({r});")
        self.emit(f"cf = ((uint64_t)({UT[size]}){a} + ({UT[size]}){b} + {carry_in}) >> {bits}; "
                  f"of = ((~({a} ^ {b}) & ({a} ^ {r})) >> {bits - 1}) & 1;")

    def flags_sub(self, a, b, r, size, borrow_in="0"):
        bits = 8 * size
        self.emit(f"zf = ({UT[size]}){r} == 0; sf = ((uint32_t){r} >> {bits - 1}) & 1; pf = parity8({r});")
        if borrow_in == "0":
            self.emit(f"cf = ({UT[size]}){a} < ({UT[size]}){b};")
        else:
            self.emit(f"cf = (uint64_t)({UT[size]}){a} < (uint64_t)({UT[size]}){b} + {borrow_in};")
        self.emit(f"of = ((({a} ^ {b}) & ({a} ^ {r})) >> {bits - 1}) & 1;")

    # ---- control flow helpers --------------------------------------------------------------
    def sync_out(self):
        self.emit("c->eax=eax; c->ecx=ecx; c->edx=edx; c->ebx=ebx; c->esp=esp; c->ebp=ebp; c->esi=esi; c->edi=edi;")

    def sync_in(self):
        self.emit("eax=c->eax; ecx=c->ecx; edx=c->edx; ebx=c->ebx; esp=c->esp; ebp=c->ebp; esi=c->esi; edi=c->edi;")

    def call_direct(self, target, ret_addr, tail=False):
        if not tail:
            self.emit(f"esp -= 4; wr32(esp, {ret_addr:#x}u);")
        self.sync_out()
        self.emit(f"F_{target:08x}(c);")
        if tail:
            self.emit("return;")
        else:
            self.sync_in()
        self.callees.add(target)

    def call_indirect(self, target_expr, ret_addr, tail=False):
        self.emit(f"{{ uint32_t t_ = {target_expr};")
        if not tail:
            self.emit(f"esp -= 4; wr32(esp, {ret_addr:#x}u);")
        self.sync_out()
        self.emit("guest_call(c, t_); }")
        if tail:
            self.emit("return;")
        else:
            self.sync_in()

    def note_import(self, op):
        """Record a call/jmp through an IAT slot. It's lifted like any indirect call: the loader points
        the slot at a host thunk (or another module's export) and guest_call dispatches on it."""
        if op.type == x86.X86_OP_MEM and not op.mem.base and not op.mem.index and (op.mem.disp & 0xffffffff) in self.p.img.imports:
            self.import_slots.add(op.mem.disp & 0xffffffff)

    def goto(self, t):
        if t not in self.body_set:
            raise Unsupported(f"jump outside body {t:#x}")
        self.labels.add(t)
        return f"goto L_{t:08x};"

    # ---- instructions ---------------------------------------------------------------------
    def lift_insn(self, i):
        m = i.mnemonic
        ops = i.operands
        nxt = i.address + i.size
        prefix = ""
        if m.startswith("rep ") or m.startswith("repz ") or m.startswith("repne "):
            prefix, m = m.split(" ", 1)
        if m.startswith("lock "):
            return self.lift_locked(m.split(" ", 1)[1], ops)
        if m == "xchg" and any(o.type == x86.X86_OP_MEM for o in ops):
            return self.lift_locked(m, ops)  # xchg with memory is implicitly locked

        if m == "nop" or m == "wait":
            return
        if m == "mov":
            self.wr(ops[0], self.rd(ops[1], ops[0].size))
        elif m in ("movzx", "movsx"):
            src = self.rd(ops[1])
            if m == "movsx":
                src = f"(int32_t)({ST[ops[1].size]}){src}"
            self.wr(ops[0], src)
        elif m == "lea":
            self.wr(ops[0], self.addr(ops[1].mem))
        elif m == "push":
            v = self.rd(ops[0], 4) if ops[0].type != x86.X86_OP_IMM else f"{ops[0].imm & 0xffffffff:#x}u"
            self.emit(f"{{ uint32_t v_ = {v}; esp -= 4; wr32(esp, v_); }}")
        elif m == "pop":
            self.emit("{ uint32_t v_ = rd32(esp); esp += 4;")
            self.wr(ops[0], "v_")
            self.emit("}")
        elif m in ("add", "sub", "adc", "sbb", "cmp"):
            sz = ops[0].size
            self.emit(f"{{ {UT[sz]} a_ = {self.rd(ops[0])}, b_ = {self.rd(ops[1], sz)};")
            if m in ("add", "adc"):
                cin = "cf" if m == "adc" else "0"
                self.emit(f"{UT[sz]} r_ = a_ + b_ + {cin};")
                self.flags_add("a_", "b_", "r_", sz, cin)
            else:
                bin_ = "cf" if m == "sbb" else "0"
                self.emit(f"{UT[sz]} r_ = a_ - b_ - {bin_};")
                self.flags_sub("a_", "b_", "r_", sz, bin_)
            if m != "cmp":
                self.wr(ops[0], "r_")
            self.emit("}")
        elif m in ("and", "or", "xor", "test"):
            sz = ops[0].size
            op = {"and": "&", "test": "&", "or": "|", "xor": "^"}[m]
            if m == "xor" and ops[0].type == x86.X86_OP_REG and ops[1].type == x86.X86_OP_REG and ops[0].reg == ops[1].reg:
                self.emit(f"{{ {UT[sz]} r_ = 0;")
            else:
                self.emit(f"{{ {UT[sz]} r_ = {self.rd(ops[0])} {op} {self.rd(ops[1], sz)};")
            self.flags_logic("r_", sz)
            if m != "test":
                self.wr(ops[0], "r_")
            self.emit("}")
        elif m in ("inc", "dec"):
            sz = ops[0].size
            self.emit(f"{{ {UT[sz]} a_ = {self.rd(ops[0])}, b_ = 1, r_ = a_ {'+' if m == 'inc' else '-'} 1; uint8_t scf_ = cf;")
            (self.flags_add if m == "inc" else self.flags_sub)("a_", "b_", "r_", sz)
            self.emit("cf = scf_;")
            self.wr(ops[0], "r_")
            self.emit("}")
        elif m == "neg":
            sz = ops[0].size
            self.emit(f"{{ {UT[sz]} a_ = 0, b_ = {self.rd(ops[0])}, r_ = a_ - b_;")
            self.flags_sub("a_", "b_", "r_", sz)
            self.wr(ops[0], "r_")
            self.emit("}")
        elif m == "not":
            self.wr(ops[0], f"~{self.rd(ops[0])}")
        elif m in ("shl", "sal", "shr", "sar", "rol", "ror"):
            sz = ops[0].size
            bits = 8 * sz
            cnt = self.rd(ops[1], 1) if len(ops) > 1 else "1"
            self.emit(f"{{ unsigned n_ = ({cnt}) & 31; if (n_) {{ {UT[sz]} a_ = {self.rd(ops[0])}, r_;")
            if m in ("shl", "sal"):
                self.emit(f"r_ = ({UT[sz]})((uint32_t)a_ << n_); cf = n_ <= {bits} ? ((uint64_t)a_ >> ({bits} - n_)) & 1 : 0;"
                          f" of = ((r_ >> {bits - 1}) & 1) ^ cf;")
            elif m == "shr":
                self.emit(f"r_ = ({UT[sz]})((uint32_t)a_ >> n_); cf = ((uint64_t)a_ >> (n_ - 1)) & 1; of = (a_ >> {bits - 1}) & 1;")
            elif m == "sar":
                self.emit(f"r_ = ({UT[sz]})(({ST[sz]})a_ >> (n_ > {bits - 1} ? {bits - 1} : n_)); "
                          f"cf = ((int64_t)({ST[sz]})a_ >> (n_ - 1)) & 1; of = 0;")
            elif m == "rol":
                self.emit(f"unsigned k_ = n_ % {bits}; r_ = ({UT[sz]})((a_ << k_) | (a_ >> (({bits} - k_) % {bits}))); cf = r_ & 1; of = ((r_ >> {bits - 1}) & 1) ^ cf;")
            elif m == "ror":
                self.emit(f"unsigned k_ = n_ % {bits}; r_ = ({UT[sz]})((a_ >> k_) | (a_ << (({bits} - k_) % {bits}))); cf = (r_ >> {bits - 1}) & 1; of = cf ^ ((r_ >> {bits - 2}) & 1);")
            if m in ("shl", "sal", "shr", "sar"):
                self.emit(f"zf = r_ == 0; sf = (r_ >> {bits - 1}) & 1; pf = parity8(r_);")
            self.wr(ops[0], "r_")
            self.emit("} }")
        elif m in ("rcl", "rcr"):
            # Rotate through carry: rotate the (bits+1)-bit value cf:a.
            sz = ops[0].size
            bits = 8 * sz
            cnt = self.rd(ops[1], 1) if len(ops) > 1 else "1"
            self.emit(f"{{ unsigned k_ = (({cnt}) & 31) % {bits + 1}; if (k_) {{ uint64_t a_ = {self.rd(ops[0])}, "
                      f"t_ = ((uint64_t)cf << {bits}) | a_, w_ = (1ull << {bits + 1}) - 1;")
            if m == "rcl":
                self.emit(f"t_ = ((t_ << k_) | (t_ >> ({bits + 1} - k_))) & w_; cf = (t_ >> {bits}) & 1; of = ((t_ >> {bits - 1}) & 1) ^ cf;")
            else:
                self.emit(f"of = ((a_ >> {bits - 1}) & 1) ^ cf; t_ = ((t_ >> k_) | (t_ << ({bits + 1} - k_))) & w_; cf = (t_ >> {bits}) & 1;")
            self.wr(ops[0], f"({UT[sz]})t_")
            self.emit("} }")
        elif m in ("shld", "shrd"):
            self.emit(f"{{ unsigned n_ = ({self.rd(ops[2], 1)}) & 31; if (n_) {{ uint32_t a_ = {self.rd(ops[0])}, b_ = {self.rd(ops[1])}, r_;")
            if m == "shld":
                self.emit("r_ = (a_ << n_) | (b_ >> (32 - n_)); cf = (a_ >> (32 - n_)) & 1;")
            else:
                self.emit("r_ = (a_ >> n_) | (b_ << (32 - n_)); cf = (a_ >> (n_ - 1)) & 1;")
            self.emit("zf = r_ == 0; sf = r_ >> 31; pf = parity8(r_); of = 0;")
            self.wr(ops[0], "r_")
            self.emit("} }")
        elif m == "imul":
            if len(ops) == 1:
                sz = ops[0].size
                if sz == 1:
                    self.emit(f"{{ int32_t r_ = (int32_t)(int8_t)eax * (int8_t){self.rd(ops[0])}; eax = (eax & 0xffff0000u) | (uint16_t)r_; cf = of = r_ != (int8_t)r_; }}")
                elif sz == 2:
                    self.emit(f"{{ int32_t r_ = (int32_t)(int16_t)eax * (int16_t){self.rd(ops[0])}; eax = (eax & 0xffff0000u) | (uint16_t)r_;"
                              f" edx = (edx & 0xffff0000u) | (uint16_t)((uint32_t)r_ >> 16); cf = of = r_ != (int16_t)r_; }}")
                else:
                    self.emit(f"{{ int64_t r_ = (int64_t)(int32_t)eax * (int32_t){self.rd(ops[0])}; eax = (uint32_t)r_; edx = (uint32_t)(r_ >> 32); cf = of = r_ != (int32_t)r_; }}")
            else:
                a, b = (ops[0], ops[1]) if len(ops) == 2 else (ops[1], ops[2])
                sz = ops[0].size
                self.emit(f"{{ int64_t r_ = (int64_t)({ST[sz]}){self.rd(a, sz)} * ({ST[sz]}){self.rd(b, sz)}; cf = of = r_ != ({ST[sz]})r_;")
                self.wr(ops[0], "r_")
                self.emit("}")
        elif m == "mul":
            if ops[0].size != 4:
                raise Unsupported("mul r/m8/16")
            self.emit(f"{{ uint64_t r_ = (uint64_t)eax * {self.rd(ops[0])}; eax = (uint32_t)r_; edx = (uint32_t)(r_ >> 32); cf = of = edx != 0; }}")
        elif m in ("div", "idiv"):
            if ops[0].size != 4:
                raise Unsupported("div r/m8/16")
            if m == "div":
                self.emit(f"{{ uint64_t n_ = ((uint64_t)edx << 32) | eax; uint32_t d_ = {self.rd(ops[0])}; eax = (uint32_t)(n_ / d_); edx = (uint32_t)(n_ % d_); }}")
            else:
                self.emit(f"{{ int64_t n_ = (int64_t)(((uint64_t)edx << 32) | eax); int32_t d_ = (int32_t){self.rd(ops[0])}; eax = (uint32_t)(int32_t)(n_ / d_); edx = (uint32_t)(int32_t)(n_ % d_); }}")
        elif m == "cdq":
            self.emit("edx = (uint32_t)((int32_t)eax >> 31);")
        elif m == "cwde":
            self.emit("eax = (uint32_t)(int32_t)(int16_t)eax;")
        elif m == "xchg":
            sz = ops[0].size
            self.emit(f"{{ {UT[sz]} a_ = {self.rd(ops[0])}, b_ = {self.rd(ops[1])};")
            self.wr(ops[0], "b_")
            self.wr(ops[1], "a_")
            self.emit("}")
        elif m == "xadd":
            sz = ops[0].size
            self.emit(f"{{ {UT[sz]} a_ = {self.rd(ops[0])}, b_ = {self.rd(ops[1])}, r_ = a_ + b_;")
            self.flags_add("a_", "b_", "r_", sz)
            self.wr(ops[1], "a_")
            self.wr(ops[0], "r_")
            self.emit("}")
        elif m == "cmpxchg":
            sz = ops[0].size
            self.emit(f"{{ {UT[sz]} a_ = ({UT[sz]})eax, b_ = {self.rd(ops[0])}, r_ = a_ - b_;")
            self.flags_sub("a_", "b_", "r_", sz)
            self.emit("if (zf) {")
            self.wr(ops[0], self.rd(ops[1]))
            self.emit("} else {")
            self.wr(x86_reg_op(self, "eax", sz), "b_")
            self.emit("} }")
        elif m == "cmpxchg8b":
            a = self.addr(ops[0].mem)
            self.emit(f"{{ uint64_t m_ = rd64({a}); zf = m_ == (((uint64_t)edx << 32) | eax);")
            self.emit(f"if (zf) wr64({a}, ((uint64_t)ecx << 32) | ebx); else {{ eax = (uint32_t)m_; edx = (uint32_t)(m_ >> 32); }} }}")
        elif m == "cpuid":
            self.emit("cpuid_fixed(eax, &eax, &ebx, &ecx, &edx);")
        elif m == "bswap":
            self.wr(ops[0], f"__builtin_bswap32({self.rd(ops[0])})")
        elif m in ("bsr", "bsf"):
            self.emit(f"{{ uint32_t s_ = {self.rd(ops[1])}; zf = s_ == 0; if (s_) {{")
            self.wr(ops[0], "31 - __builtin_clz(s_)" if m == "bsr" else "__builtin_ctz(s_)")
            self.emit("} }")
        elif m == "bts":
            if ops[0].type != x86.X86_OP_REG:
                raise Unsupported("bts mem")
            self.emit(f"{{ unsigned b_ = {self.rd(ops[1], 1)} & 31; uint32_t v_ = {self.rd(ops[0])}; cf = (v_ >> b_) & 1;")
            self.wr(ops[0], "v_ | (1u << b_)")
            self.emit("}")
        elif m.startswith("set"):
            self.wr(ops[0], CC[m[3:]])
        elif m.startswith("cmov"):
            self.emit(f"if ({CC[m[4:]]}) {{")
            self.wr(ops[0], self.rd(ops[1]))
            self.emit("}")
        elif m == "pushfd":  # IF set, AF not modelled
            self.emit("{ uint32_t v_ = cf | 2 | (pf<<2) | (zf<<6) | (sf<<7) | 0x200 | (of<<11); esp -= 4; wr32(esp, v_); }")
        elif m == "lahf":
            self.emit("eax = (eax & ~0xff00u) | ((uint32_t)((sf<<7)|(zf<<6)|(pf<<2)|2|cf) << 8);")
        elif m == "sahf":
            self.emit("{ uint32_t h_ = eax >> 8; sf = (h_>>7)&1; zf = (h_>>6)&1; pf = (h_>>2)&1; cf = h_&1; }")
        elif m == "leave":
            self.emit("esp = ebp; ebp = rd32(esp); esp += 4;")
        elif m in ("movsd", "stosd", "stosb", "stosw", "movsb", "movsw") and ("es:[edi]" in i.op_str):
            self.string_op(m, prefix)
        elif m == "jmp":
            self.lift_jmp(i)
        elif m.startswith("j") and m[1:] in CC:
            self.emit(f"if ({CC[m[1:]]}) {self.goto(ops[0].imm & 0xffffffff)}")
        elif m == "jecxz":
            self.emit(f"if (ecx == 0) {self.goto(ops[0].imm & 0xffffffff)}")
        elif m == "call":
            self.lift_call(i)
        elif m == "ret":
            n = ops[0].imm if ops else 0
            self.emit(f"esp += {4 + n};")
            self.sync_out()
            self.emit("return;")
        elif m in ("int3", "ud2", "hlt", "int"):
            self.emit(f"guest_unimpl(c, {i.address:#x}u, \"{m}\");")
        elif m.startswith("f") and m not in ("fs",):
            self.lift_x87(i, m, ops)
        else:
            self.lift_sse(i, m, ops)

    def lift_locked(self, m, ops):
        """Locked read-modify-write on memory, via the lk_* atomic helpers in cpu.h."""
        mem = ops[0] if ops[0].type == x86.X86_OP_MEM else ops[1]
        sz = mem.size
        a = self.addr(mem.mem)
        n = 8 * sz
        if m == "xchg":
            reg = ops[1] if mem is ops[0] else ops[0]
            self.wr(reg, f"lk_xchg{n}({a}, {self.rd(reg)})")
        elif m == "xadd":
            self.emit(f"{{ {UT[sz]} b_ = {self.rd(ops[1])}, a_ = lk_add{n}({a}, b_), r_ = a_ + b_;")
            self.flags_add("a_", "b_", "r_", sz)
            self.wr(ops[1], "a_")
            self.emit("}")
        elif m == "cmpxchg":
            self.emit(f"{{ {UT[sz]} a_ = ({UT[sz]})eax, b_ = lk_cas{n}({a}, a_, {self.rd(ops[1])}), r_ = a_ - b_;")
            self.flags_sub("a_", "b_", "r_", sz)
            self.emit("if (!zf) {")
            self.wr(x86_reg_op(self, "eax", sz), "b_")
            self.emit("} }")
        elif m == "cmpxchg8b":
            self.emit(f"{{ uint64_t e_ = ((uint64_t)edx << 32) | eax, m_ = lk_cas64({a}, e_, ((uint64_t)ecx << 32) | ebx);"
                      " zf = m_ == e_; eax = (uint32_t)m_; edx = (uint32_t)(m_ >> 32); }")
        elif m in ("add", "sub", "inc", "dec", "and", "or", "xor"):
            b = "1" if m in ("inc", "dec") else self.rd(ops[1], sz)
            fn = {"inc": "add", "dec": "sub"}.get(m, m)
            op = {"add": "+", "sub": "-", "and": "&", "or": "|", "xor": "^"}[fn]
            self.emit(f"{{ {UT[sz]} b_ = {b}, a_ = lk_{fn}{n}({a}, b_), r_ = a_ {op} b_; uint8_t scf_ = cf;")
            if fn in ("and", "or", "xor"):
                self.flags_logic("r_", sz)
            else:
                (self.flags_add if fn == "add" else self.flags_sub)("a_", "b_", "r_", sz)
            if m in ("inc", "dec"):
                self.emit("cf = scf_;")
            self.emit("}")
        else:
            raise Unsupported(f"lock {m}")

    def string_op(self, m, prefix):
        sz = {"b": 1, "w": 2, "d": 4}[m[-1]]
        body = {"movs": f"{WR[sz]}(edi, {RD[sz]}(esi)); esi += {sz}; edi += {sz};",
                "stos": f"{WR[sz]}(edi, ({UT[sz]})eax); edi += {sz};"}[m[:-1]]
        if prefix == "rep":
            self.emit(f"while (ecx) {{ {body} ecx--; }}")
        elif prefix:
            raise Unsupported(prefix + " " + m)
        else:
            self.emit(body)

    def lift_jmp(self, i):
        op = i.operands[0]
        if op.type == x86.X86_OP_IMM:
            t = op.imm & 0xffffffff
            if self.p.is_func(t) and t != self.start:
                self.call_direct(t, 0, tail=True)
            else:
                self.emit(self.goto(t))
        elif i.address in self.p.jump_tables:
            tbl, tgts = self.p.jump_tables[i.address]
            idx = self.reg_name(op.mem.index)
            self.emit(f"switch ({idx}) {{")
            for k, t in enumerate(tgts):
                self.emit(f"case {k}: {self.goto(t)}")
            self.emit(f"default: guest_unimpl(c, {i.address:#x}u, \"switch index\"); return; }}")
        else:
            self.note_import(op)
            self.call_indirect(self.rd(op, 4), 0, tail=True)

    def lift_call(self, i):
        op = i.operands[0]
        ret = i.address + i.size
        if op.type == x86.X86_OP_IMM:
            self.call_direct(op.imm & 0xffffffff, ret)
        else:
            self.note_import(op)
            self.call_indirect(self.rd(op, 4), ret)

    def st_index(self, op):
        n = self.reg_name(op.reg)
        mm = re.fullmatch(r"st\((\d)\)", n)
        if not mm:
            raise Unsupported(f"x87 reg {n}")
        return int(mm.group(1))

    def fmem(self, op):
        """Read a memory operand of an x87 arithmetic/load instruction as double."""
        a = self.addr(op.mem)
        return {4: f"(double)rdf32({a})", 8: f"rdf64({a})"}[op.size]

    def fimem(self, op):
        a = self.addr(op.mem)
        return {2: f"(double)(int16_t)rd16({a})", 4: f"(double)(int32_t)rd32({a})", 8: f"(double)(int64_t)rd64({a})"}[op.size]

    def lift_x87(self, i, m, ops):
        isst = lambda o: o.type == x86.X86_OP_REG  # noqa: E731
        arith = {"add": "a_ + b_", "sub": "a_ - b_", "subr": "b_ - a_", "mul": "a_ * b_", "div": "a_ / b_", "divr": "b_ / a_"}
        if m in ("fld", "fild"):
            if isst(ops[0]):
                self.emit(f"st_push(c, ST({self.st_index(ops[0])}));")
            else:
                if m == "fld" and ops[0].size == 10:
                    raise Unsupported("fld m80")
                self.emit(f"st_push(c, {self.fmem(ops[0]) if m == 'fld' else self.fimem(ops[0])});")
        elif m in ("fldz", "fld1"):
            self.emit(f"st_push(c, {'0.0' if m == 'fldz' else '1.0'});")
        elif m in ("fst", "fstp"):
            if isst(ops[0]):
                self.emit(f"ST({self.st_index(ops[0])}) = ST(0);")
            elif ops[0].size == 4:
                self.emit(f"wrf32({self.addr(ops[0].mem)}, (float)ST(0));")
            elif ops[0].size == 8:
                self.emit(f"wrf64({self.addr(ops[0].mem)}, ST(0));")
            else:
                raise Unsupported("fstp m80")
            if m == "fstp":
                self.emit("st_pop(c);")
        elif m in ("fist", "fistp", "fisttp"):
            sz = ops[0].size
            v = "__builtin_trunc(ST(0))" if m == "fisttp" else "fpu_round(c, ST(0))"
            self.emit(f"{WR[sz]}({self.addr(ops[0].mem)}, ({UT[sz]})fist64({v}, {sz * 8}));")
            if m != "fist":
                self.emit("st_pop(c);")
        elif m.rstrip("p") in ("fadd", "fsub", "fsubr", "fmul", "fdiv", "fdivr") or m in ("fiadd", "fisub", "fisubr", "fimul", "fidiv", "fidivr"):
            pop = m.endswith("p") and m not in ("fsubr", "fdivr") or m in ("faddp", "fsubp", "fsubrp", "fmulp", "fdivp", "fdivrp")
            base = m[2:] if m.startswith("fi") else m[1:]
            if pop:
                base = base[:-1]
            expr = arith[base]
            if len(ops) == 1 and not isst(ops[0]):
                src = self.fimem(ops[0]) if m.startswith("fi") else self.fmem(ops[0])
                self.emit(f"{{ double a_ = ST(0), b_ = {src}; ST(0) = {expr}; }}")
            else:
                if len(ops) == 1:  # "fop st(i)" is st(0) op= st(i); "fopp st(i)" is st(i) op= st(0), then pop
                    d, s_ = (self.st_index(ops[0]), 0) if pop else (0, self.st_index(ops[0]))
                else:
                    d, s_ = self.st_index(ops[0]), self.st_index(ops[1])
                if len(ops) == 0:
                    d, s_ = 1, 0
                self.emit(f"{{ double a_ = ST({d}), b_ = ST({s_}); ST({d}) = {expr}; }}")
                if pop:
                    self.emit("st_pop(c);")
        elif m in ("fchs", "fabs", "fsqrt"):
            f = {"fchs": "-ST(0)", "fabs": "__builtin_fabs(ST(0))", "fsqrt": "__builtin_sqrt(ST(0))"}[m]
            self.emit(f"ST(0) = {f};")
        elif m == "fxch":
            k = self.st_index(ops[-1]) if ops else 1  # capstone gives "fxch st(i)" the operands st(0), st(i)
            self.emit(f"{{ double t_ = ST(0); ST(0) = ST({k}); ST({k}) = t_; }}")
        elif m in ("fcomi", "fucomi", "fcomip", "fucomip"):
            k = self.st_index(ops[1] if len(ops) > 1 else ops[0])
            self.emit(f"{{ double a_ = ST(0), b_ = ST({k}); int un_ = a_ != a_ || b_ != b_; zf = un_ || a_ == b_; pf = un_; cf = un_ || a_ < b_; of = sf = 0; }}")
            if m.endswith("p"):
                self.emit("st_pop(c);")
        elif m in ("fcom", "fcomp", "fcompp", "fucom", "fucomp", "fucompp", "ficom", "ficomp"):
            if ops and not isst(ops[-1]):
                src = self.fimem(ops[0]) if m.startswith("fi") else self.fmem(ops[0])
            else:
                src = f"ST({self.st_index(ops[-1]) if ops else 1})"
            self.emit(f"c->fpu_sw = fcom_bits(ST(0), {src});")
            pops = 2 if m.endswith("pp") else 1 if m.endswith("p") else 0
            self.emit("st_pop(c);" * pops)
        elif m == "fnstsw":
            self.wr(ops[0], "(uint16_t)(c->fpu_sw | ((c->st_top & 7) << 11))", 2)
        elif m == "fnstcw":
            self.emit(f"wr16({self.addr(ops[0].mem)}, c->fpu_cw);")
        elif m == "fldcw":
            self.emit(f"c->fpu_cw = rd16({self.addr(ops[0].mem)});")
        elif m.startswith("fcmov"):
            cc = {"fcmovb": "cf", "fcmovnb": "!cf", "fcmove": "zf", "fcmovne": "!zf", "fcmovbe": "(cf|zf)",
                  "fcmovnbe": "!(cf|zf)", "fcmovu": "pf", "fcmovnu": "!pf"}[m]
            self.emit(f"if ({cc}) ST(0) = ST({self.st_index(ops[1])});")
        else:
            raise Unsupported(m)

    def lift_sse(self, i, m, ops):
        X = self.xmm
        isx = lambda o: o.type == x86.X86_OP_REG and self.reg_name(o.reg).startswith("xmm")  # noqa: E731

        def src32f(o):
            return f"{X(o)}.f32[0]" if isx(o) else f"rdf32({self.addr(o.mem)})"

        def src64f(o):
            return f"{X(o)}.f64[0]" if isx(o) else f"rdf64({self.addr(o.mem)})"

        def src128(o):
            return X(o) if isx(o) else f"(*(Xmm *)P({self.addr(o.mem)}))"

        def lanes(n, f, expr):
            """dst.f[k] = expr for each of n lanes; expr sees a_ (old dst) and b_ (source)."""
            self.emit(f"{{ Xmm a_ = {X(ops[0])}, b_ = {src128(ops[1])};"
                      f" for (int k_ = 0; k_ < {n}; k_++) {X(ops[0])}.{f}[k_] = {expr}; }}")

        def shift_count(o):
            if o.type == x86.X86_OP_IMM:
                return f"{o.imm & 0xff}ull"
            return f"{X(o)}.u64[0]" if isx(o) else f"rd64({self.addr(o.mem)})"

        cmp_pred = {"eq": "a_ == b_", "lt": "a_ < b_", "le": "a_ <= b_", "unord": "(a_ != a_ || b_ != b_)",
                    "neq": "!(a_ == b_)", "nlt": "!(a_ < b_)", "nle": "!(a_ <= b_)", "ord": "!(a_ != a_ || b_ != b_)"}
        mm = re.fullmatch(r"cmp(\w+?)(ss|sd)", m)
        if mm and mm.group(1) in cmp_pred:
            if mm.group(2) == "ss":
                a, b, d = f"{X(ops[0])}.f32[0]", src32f(ops[1]), f"{X(ops[0])}.u32[0]"
            else:
                a, b, d = f"{X(ops[0])}.f64[0]", src64f(ops[1]), f"{X(ops[0])}.u64[0]"
            self.emit(f"{{ __typeof__({a}) a_ = {a}, b_ = {b}; {d} = ({cmp_pred[mm.group(1)]}) ? ~(__typeof__({d}))0 : 0; }}")
            return
        packed_int = {
            "paddb": (16, "b", "a_.b[k_] + b_.b[k_]"),
            "paddw": (8, "u16", "a_.u16[k_] + b_.u16[k_]"),
            "paddd": (4, "u32", "a_.u32[k_] + b_.u32[k_]"),
            "paddq": (2, "u64", "a_.u64[k_] + b_.u64[k_]"),
            "psubw": (8, "u16", "a_.u16[k_] - b_.u16[k_]"),
            "psubd": (4, "u32", "a_.u32[k_] - b_.u32[k_]"),
            "psubq": (2, "u64", "a_.u64[k_] - b_.u64[k_]"),
            "pcmpgtd": (4, "u32", "a_.i32[k_] > b_.i32[k_] ? 0xffffffffu : 0"),
            "pabsd": (4, "u32", "b_.i32[k_] < 0 ? -b_.u32[k_] : b_.u32[k_]"),
            "pmulhw": (8, "u16", "(uint16_t)(((int32_t)a_.i16[k_] * b_.i16[k_]) >> 16)"),
            "pmaddwd": (4, "u32", "(uint32_t)((int32_t)a_.i16[2*k_] * b_.i16[2*k_]) + (uint32_t)((int32_t)a_.i16[2*k_+1] * b_.i16[2*k_+1])"),
            "packssdw": (8, "i16", "sat16(k_ < 4 ? a_.i32[k_] : b_.i32[k_ - 4])"),
            "packuswb": (16, "b", "usat8(k_ < 8 ? a_.i16[k_] : b_.i16[k_ - 8])"),
        }
        shifts = {"psllw": (8, "u16", "<<", 16), "psrlw": (8, "u16", ">>", 16), "psraw": (8, "i16", ">>", 16),
                  "psrad": (4, "i32", ">>", 32), "psllq": (2, "u64", "<<", 64), "psrlq": (2, "u64", ">>", 64)}
        if m in packed_int:
            lanes(*packed_int[m])
            return
        if m in shifts:
            n, f, o, bits = shifts[m]
            if f.startswith("i"):  # arithmetic: counts past the width fill with the sign bit
                v = f"{X(ops[0])}.{f}[k_] >> (n_ > {bits - 1} ? {bits - 1} : n_)"
            else:
                v = f"n_ > {bits - 1} ? 0 : {X(ops[0])}.{f}[k_] {o} n_"
            self.emit(f"{{ uint64_t n_ = {shift_count(ops[1])}; for (int k_ = 0; k_ < {n}; k_++) {X(ops[0])}.{f}[k_] = {v}; }}")
            return

        if m == "movss":
            if isx(ops[0]) and isx(ops[1]):
                self.emit(f"{X(ops[0])}.f32[0] = {X(ops[1])}.f32[0];")
            elif isx(ops[0]):
                self.emit(f"{X(ops[0])}.u64[1] = 0; {X(ops[0])}.u32[1] = 0; {X(ops[0])}.f32[0] = rdf32({self.addr(ops[1].mem)});")
            else:
                self.emit(f"wrf32({self.addr(ops[0].mem)}, {X(ops[1])}.f32[0]);")
        elif m == "movsd":
            if isx(ops[0]) and isx(ops[1]):
                self.emit(f"{X(ops[0])}.f64[0] = {X(ops[1])}.f64[0];")
            elif isx(ops[0]):
                self.emit(f"{X(ops[0])}.u64[1] = 0; {X(ops[0])}.u64[0] = rd64({self.addr(ops[1].mem)});")
            else:
                self.emit(f"wr64({self.addr(ops[0].mem)}, {X(ops[1])}.u64[0]);")
        elif m in ("movaps", "movups", "movdqu", "movdqa", "movapd", "movupd"):
            if isx(ops[0]):
                self.emit(f"{{ Xmm t_; memcpy(&t_, &{src128(ops[1])}, 16); {X(ops[0])} = t_; }}")
            else:
                self.emit(f"memcpy(P({self.addr(ops[0].mem)}), &{X(ops[1])}, 16);")
        elif m in ("movd",):
            if isx(ops[0]):
                self.emit(f"{X(ops[0])}.u64[1] = 0; {X(ops[0])}.u64[0] = {self.rd(ops[1], 4)};")
            else:
                self.wr(ops[0], f"{X(ops[1])}.u32[0]", 4)
        elif m == "movq":
            if isx(ops[0]):
                s = f"{X(ops[1])}.u64[0]" if isx(ops[1]) else f"rd64({self.addr(ops[1].mem)})"
                self.emit(f"{X(ops[0])}.u64[0] = {s}; {X(ops[0])}.u64[1] = 0;")
            else:
                self.emit(f"wr64({self.addr(ops[0].mem)}, {X(ops[1])}.u64[0]);")
        elif m in ("addss", "subss", "mulss", "divss"):
            o = {"add": "+", "sub": "-", "mul": "*", "div": "/"}[m[:3]]
            self.emit(f"{X(ops[0])}.f32[0] = {X(ops[0])}.f32[0] {o} {src32f(ops[1])};")
        elif m in ("addsd", "subsd", "mulsd", "divsd"):
            o = {"add": "+", "sub": "-", "mul": "*", "div": "/"}[m[:3]]
            self.emit(f"{X(ops[0])}.f64[0] = {X(ops[0])}.f64[0] {o} {src64f(ops[1])};")
        elif m in ("mulps", "subps", "mulpd", "subpd", "maxps"):
            n = 4 if m.endswith("ps") else 2
            f = "f32" if n == 4 else "f64"
            self.emit(f"{{ Xmm s_ = {src128(ops[1])};")
            for k in range(n):
                d = f"{X(ops[0])}.{f}[{k}]"
                if m.startswith("max"):
                    self.emit(f"{d} = {d} > s_.{f}[{k}] ? {d} : s_.{f}[{k}];")
                else:
                    self.emit(f"{d} = {d} {'*' if m.startswith('mul') else '-'} s_.{f}[{k}];")
            self.emit("}")
        elif m in ("xorps", "xorpd", "andpd", "orps", "pxor", "pand", "por", "pandn"):
            o = {"xorps": "^", "xorpd": "^", "pxor": "^", "andpd": "&", "pand": "&", "pandn": "&",
                 "orps": "|", "por": "|"}[m]
            if isx(ops[1]) and ops[0].reg == ops[1].reg and o == "^":
                self.emit(f"{X(ops[0])}.u64[0] = 0; {X(ops[0])}.u64[1] = 0;")
            else:
                self.emit(f"{{ Xmm s_ = {src128(ops[1])};")
                for k in range(2):
                    d = f"{X(ops[0])}.u64[{k}]"
                    self.emit(f"{d} = {'~' + d if m == 'pandn' else d} {o} s_.u64[{k}];")
                self.emit("}")
        elif m in ("comiss", "ucomiss", "comisd", "ucomisd"):
            if m.endswith("ss"):
                a, b = f"{X(ops[0])}.f32[0]", src32f(ops[1])
            else:
                a, b = f"{X(ops[0])}.f64[0]", src64f(ops[1])
            self.emit(f"{{ __typeof__({a}) a_ = {a}, b_ = {b}; int un_ = a_ != a_ || b_ != b_;"
                      f" zf = un_ || a_ == b_; pf = un_; cf = un_ || a_ < b_; of = 0; sf = 0; }}")
        elif m == "cvttss2si":
            self.wr(ops[0], f"cvtt_f32_i32({src32f(ops[1])})")
        elif m == "cvttsd2si":
            self.wr(ops[0], f"cvtt_f64_i32({src64f(ops[1])})")
        elif m == "cvtss2sd":
            self.emit(f"{X(ops[0])}.f64[0] = (double){src32f(ops[1])};")
        elif m == "cvtsd2ss":
            self.emit(f"{X(ops[0])}.f32[0] = (float){src64f(ops[1])};")
        elif m == "cvtsi2sd":
            self.emit(f"{X(ops[0])}.f64[0] = (double)(int32_t){self.rd(ops[1], 4)};")
        elif m == "cvtdq2ps":
            self.emit(f"{{ Xmm s_ = {src128(ops[1])}; for (int k_ = 0; k_ < 4; k_++) {X(ops[0])}.f32[k_] = (float)(int32_t)s_.u32[k_]; }}")
        elif m == "cvtdq2pd":
            s = f"{X(ops[1])}" if isx(ops[1]) else f"(*(Xmm *)P({self.addr(ops[1].mem)}))"
            self.emit(f"{{ int32_t a_ = (int32_t){s}.u32[0], b_ = (int32_t){s}.u32[1]; {X(ops[0])}.f64[0] = a_; {X(ops[0])}.f64[1] = b_; }}")
        elif m == "cvtps2pd":
            s = f"{X(ops[1])}" if isx(ops[1]) else f"(*(Xmm *)P({self.addr(ops[1].mem)}))"
            self.emit(f"{{ float a_ = {s}.f32[0], b_ = {s}.f32[1]; {X(ops[0])}.f64[0] = a_; {X(ops[0])}.f64[1] = b_; }}")
        elif m == "cvtpd2ps":
            self.emit(f"{{ Xmm s_ = {src128(ops[1])}; {X(ops[0])}.f32[0] = (float)s_.f64[0]; {X(ops[0])}.f32[1] = (float)s_.f64[1]; {X(ops[0])}.u64[1] = 0; }}")
        elif m in ("unpcklps", "unpcklpd", "unpckhpd", "unpckhps"):
            self.emit(f"{{ Xmm a_ = {X(ops[0])}, b_ = {src128(ops[1])};")
            if m == "unpcklps":
                self.emit(f"{X(ops[0])}.u32[0] = a_.u32[0]; {X(ops[0])}.u32[1] = b_.u32[0]; {X(ops[0])}.u32[2] = a_.u32[1]; {X(ops[0])}.u32[3] = b_.u32[1]; }}")
            elif m == "unpckhps":
                self.emit(f"{X(ops[0])}.u32[0] = a_.u32[2]; {X(ops[0])}.u32[1] = b_.u32[2]; {X(ops[0])}.u32[2] = a_.u32[3]; {X(ops[0])}.u32[3] = b_.u32[3]; }}")
            elif m == "unpcklpd":
                self.emit(f"{X(ops[0])}.u64[0] = a_.u64[0]; {X(ops[0])}.u64[1] = b_.u64[0]; }}")
            else:
                self.emit(f"{X(ops[0])}.u64[0] = a_.u64[1]; {X(ops[0])}.u64[1] = b_.u64[1]; }}")
        elif m == "shufps":
            imm = ops[2].imm
            self.emit(f"{{ Xmm a_ = {X(ops[0])}, b_ = {src128(ops[1])};")
            self.emit(f"{X(ops[0])}.u32[0] = a_.u32[{imm & 3}]; {X(ops[0])}.u32[1] = a_.u32[{(imm >> 2) & 3}]; "
                      f"{X(ops[0])}.u32[2] = b_.u32[{(imm >> 4) & 3}]; {X(ops[0])}.u32[3] = b_.u32[{(imm >> 6) & 3}]; }}")
        elif m == "psrldq":
            n = ops[1].imm
            self.emit(f"{{ Xmm a_ = {X(ops[0])}, r_ = {{0}}; for (int k_ = 0; k_ + {n} < 16; k_++) r_.b[k_] = a_.b[k_ + {n}]; {X(ops[0])} = r_; }}")
        elif m in ("punpcklbw", "punpcklwd", "punpckldq", "punpcklqdq", "punpckhbw", "punpckhwd", "punpckhdq", "punpckhqdq"):
            esz = {"bw": 1, "wd": 2, "dq": 4, "qdq": 8}[m[7:]]
            self.emit(f"{X(ops[0])} = xmm_unpack({X(ops[0])}, {src128(ops[1])}, {esz}, {int(m[6] == 'h')});")
        elif m == "pmovsxbd":
            s = f"{X(ops[1])}.u32[0]" if isx(ops[1]) else f"rd32({self.addr(ops[1].mem)})"
            self.emit(f"{{ uint32_t s_ = {s}; for (int k_ = 0; k_ < 4; k_++) {X(ops[0])}.u32[k_] = (uint32_t)(int32_t)(int8_t)(s_ >> (8 * k_)); }}")
        elif m == "pshufd":
            imm = ops[2].imm
            self.emit(f"{{ Xmm s_ = {src128(ops[1])};"
                      + "".join(f" {X(ops[0])}.u32[{k}] = s_.u32[{(imm >> (2 * k)) & 3}];" for k in range(4)) + " }")
        elif m == "pslldq":
            n = ops[1].imm
            self.emit(f"{{ Xmm a_ = {X(ops[0])}, r_ = {{0}}; for (int k_ = {n}; k_ < 16; k_++) r_.b[k_] = a_.b[k_ - {n}]; {X(ops[0])} = r_; }}")
        elif m == "pinsrw":
            self.emit(f"{X(ops[0])}.u16[{ops[2].imm & 7}] = {self.rd(ops[1], 2)};")
        elif m == "movlpd":
            if isx(ops[0]):
                self.emit(f"{X(ops[0])}.u64[0] = rd64({self.addr(ops[1].mem)});")
            else:
                self.emit(f"wr64({self.addr(ops[0].mem)}, {X(ops[1])}.u64[0]);")
        else:
            raise Unsupported(m)

    # ---- function --------------------------------------------------------------------------
    def lift(self):
        body = self.p.function_body(self.start)
        self.body_set = set(body)
        self.callees = set()
        out = []
        prev_falls_to = None
        for a in body:
            i = self.p.decode(a)
            self.cur = i
            if prev_falls_to is not None and prev_falls_to != a:
                self.emit(self.goto(prev_falls_to))
            self.lines.append(f"L_{a:08x}: ;  // {i.mnemonic} {i.op_str}")
            try:
                self.lift_insn(i)
            except Unsupported as e:
                raise Unsupported(f"{a:#x} {i.mnemonic} {i.op_str}: {e}")
            m = i.mnemonic
            ends = m in ("jmp", "ret", "int3", "ud2", "hlt") or m.startswith("ret")
            if self.p.is_noreturn_call(a):
                self.emit(f"guest_unimpl(c, {a:#x}u, \"noreturn import returned\"); return;")
                ends = True
            prev_falls_to = None if ends else a + i.size
        if prev_falls_to is not None:
            if self.p.is_func(prev_falls_to):
                self.call_direct(prev_falls_to, 0, tail=True)
            else:
                raise Unsupported(f"falls off end at {prev_falls_to:#x}")
        self.labels.add(self.start)
        # Drop labels nobody jumps to.
        text = []
        for ln in self.lines:
            mm = re.match(r"L_([0-9a-f]{8}): ;", ln)
            if mm and int(mm.group(1), 16) not in self.labels:
                text.append("  /* " + ln.split("// ", 1)[1] + " */")
            else:
                text.append(ln)
        out.append(f"void F_{self.start:08x}(CPU *restrict c) {{")
        out.append("  uint32_t eax=c->eax, ecx=c->ecx, edx=c->edx, ebx=c->ebx, esp=c->esp, ebp=c->ebp, esi=c->esi, edi=c->edi;")
        out.append("  uint8_t cf=0, zf=0, sf=0, of=0, pf=0; (void)cf; (void)zf; (void)sf; (void)of; (void)pf;")
        if body[0] != self.start:
            out.append(f"  goto L_{self.start:08x};")
        out.extend(text)
        out.append("}")
        return "\n".join(out)


def x86_reg_op(lifter, name, size):
    """A fake register operand for writing eax/ax/al in cmpxchg."""
    class Op:
        type = x86.X86_OP_REG
        reg = {4: x86.X86_REG_EAX, 2: x86.X86_REG_AX, 1: x86.X86_REG_AL}[size]
    Op.size = size
    return Op


def lift_one(prog, addr):
    return FnLifter(prog, addr).lift()


if __name__ == "__main__":
    prog = Program()
    for a in sys.argv[1:]:
        print(lift_one(prog, int(a, 16)))
