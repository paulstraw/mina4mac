"""Per-instruction differential test: Unicorn vs the lifter's C for single instructions.

Samples real instances of the given mnemonics from noita.exe, runs each one alone (followed by
`pushfd; pop [FLAGS]` so defined flags are compared too) on random inputs, and compares
registers, xmm, flags and memory. Covers instructions that whole-function difftest rarely reaches.

  uv run tools/insntest.py [--per 20] [--trials 20] [--seed S] [mnemonic ...]
  uv run tools/insntest.py --at 42b24f,8650f7   (specific instruction addresses)
  uv run tools/insntest.py --x87reg   (synthetic x87 register forms: fld x3; op; fstp to memory)
"""
import argparse
import random
import struct
import subprocess
import sys
from pathlib import Path

import unicorn as uc
from unicorn import x86_const as X

sys.path.insert(0, str(Path(__file__).parent))
from difftest import (BUILD, GEN, MOD, NAMES, SCRATCH_LO, SCRATCH_SZ, SENTINEL, STACK_LO, STACK_SZ, TEB,  # noqa: E402
                      Ref, rand_block, rand_value, run_native)
from lift import FnLifter, Program, ROOT, Unsupported  # noqa: E402

CODE = 0x0C000000
FLAGS = SCRATCH_LO + SCRATCH_SZ - 4  # where the test stub stores eflags
CF, PF, ZF, SF, OF = 0x1, 0x4, 0x40, 0x80, 0x800
# Flags compared per mnemonic: the ones it defines. Anything not listed must leave eflags untouched,
# so the whole word is compared, which also checks the lifted pushfd's encoding.
ARITH = CF | PF | ZF | SF | OF  # AF isn't modelled
FLAG_MASK = {"imul": CF | OF, "rcr": CF | OF, "rcl": CF | OF, "cmpxchg8b": ZF,
             "xadd": ARITH, "cmpxchg": ARITH, "add": ARITH, "sub": ARITH, "inc": ARITH, "dec": ARITH}
DEFAULT = ["imul", "xadd", "xchg", "cmpxchg", "cmpxchg8b", "rcr", "cpuid", "punpcklbw", "punpcklwd", "punpckldq", "punpckhbw",
           "punpckhwd", "psllq", "psrlq", "psllw", "psrlw", "psraw", "psrad", "pslldq", "paddb", "paddw",
           "paddq", "psubw", "psubd", "psubq", "pcmpgtd", "pabsd", "pmulhw", "pmaddwd", "packssdw",
           "packuswb", "pinsrw", "pshufd", "pmovsxbd", "cmpeqsd", "orps", "pxor", "pand", "pandn", "por",
           "xorps", "xorpd", "andpd"]
TAIL = b"\x9c\x8f\x05" + struct.pack("<I", FLAGS) + b"\xc3"  # pushfd; pop dword [FLAGS]; ret
STRIDE = 64  # bytes per test in CODE
# x87 register forms, which the binary's own instances can't test (x87 registers aren't compared). Each runs
# as: fld dword [SCRATCH_LO + 0/4/8]; op; fstp dword [SCRATCH_LO + 16 + 4k] for every value left.
X87_REG = {"d8c1": 0, "d8c9": 0, "d8ca": 0, "d8e1": 0, "d8e9": 0, "d8f1": 0, "d8f9": 0, "dcc1": 0, "dcc9": 0,
           "dcca": 0, "dce1": 0, "dce9": 0, "dcf1": 0, "dcf9": 0, "dec1": 1, "dec9": 1, "deca": 1, "dee1": 1,
           "dee9": 1, "def1": 1, "def9": 1, "d9c9": 0, "d9ca": 0, "ddd9": 1, "ddda": 1, "ddd1": 0}  # hex: pops


def x87_reg_code(op, pops):
    loads = b"".join(b"\xd9\x05" + struct.pack("<I", SCRATCH_LO + 4 * k) for k in range(3))
    stores = b"".join(b"\xd9\x1d" + struct.pack("<I", SCRATCH_LO + 16 + 4 * k) for k in range(3 - pops))
    return loads + bytes.fromhex(op) + stores


def base_mnemonic(m):
    return m.split(" ", 1)[1] if m.startswith("lock ") else m


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--per", type=int, default=20, help="instances per mnemonic")
    ap.add_argument("--trials", type=int, default=20)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--at", type=lambda v: [int(x, 16) for x in v.split(",")], default=[],
                    help="comma-separated hex addresses of specific instructions to test instead")
    ap.add_argument("--x87reg", action="store_true", help="test the synthetic x87 register forms (X87_REG) instead")
    ap.add_argument("mnemonics", nargs="*", default=DEFAULT)
    args = ap.parse_args()
    rng = random.Random(args.seed)
    prog = Program()

    by_m = {}
    for a, (_, m, _) in sorted(prog.insns.items()):
        if base_mnemonic(m) in args.mnemonics:
            by_m.setdefault(base_mnemonic(m), []).append(a)
    tests = []  # (test addr, original insn addr, mnemonic, code bytes without TAIL)
    for a in args.at:
        tests.append((CODE + STRIDE * len(tests), a, base_mnemonic(prog.insns[a][1]), prog.img.read(a, prog.insns[a][0])))
    for op, pops in X87_REG.items() if args.x87reg else ():
        code = x87_reg_code(op, pops)
        i = next(prog.md.disasm(bytes.fromhex(op), 0))
        tests.append((CODE + STRIDE * len(tests), None, f"{i.mnemonic} {i.op_str}", code))
    for m in [] if args.at or args.x87reg else args.mnemonics:
        insts = by_m.get(m, [])
        if not insts:
            print(f"  {m}: no instances in the binary")
        for a in rng.sample(insts, min(args.per, len(insts))):
            tests.append((CODE + STRIDE * len(tests), a, m, prog.img.read(a, prog.insns[a][0])))

    # Native: one function per test, the lifted instruction then the same flags tail.
    out = ['#include "rt.h"']
    for t, a, m, code in tests:
        lf = FnLifter(prog, t)
        lf.body_set, lf.callees = set(), set()
        for ins in prog.md.disasm(bytes(code) + TAIL, t):
            lf.cur = ins
            try:
                lf.lift_insn(ins)
            except Unsupported as e:
                sys.exit(f"{m} ({ins.mnemonic} {ins.op_str}): unsupported: {e}")
        out.append(f"void F_{t:08x}(CPU *restrict c) {{")
        out.append("  uint32_t eax=c->eax, ecx=c->ecx, edx=c->edx, ebx=c->ebx, esp=c->esp, ebp=c->ebp, esi=c->esi, edi=c->edi;")
        out.append("  uint8_t cf=0, zf=0, sf=0, of=0, pf=0;")
        lf.finish_syncs(mode="full")
        out.extend(lf.lines)
        out.append("}")
    out.append("const FnEntry FN_TABLE[] = {" + ",".join(f"{{{t:#x}u,F_{t:08x}}}" for t, *_ in tests) + "};")
    out.append(f"const int FN_COUNT = {len(tests)};")
    out.append("void guest_call(CPU *c, uint32_t t);")
    GEN.mkdir(parents=True, exist_ok=True)
    (GEN / "insntest.c").write_text("\n".join(out))
    exe = BUILD / "insntest"
    subprocess.run(["clang", "-O2", "-ffp-contract=off", "-fno-strict-aliasing", "-w", "-I", str(ROOT / "runtime"),
                    str(ROOT / "runtime/harness.c"), str(ROOT / "runtime/rt.c"), str(GEN / "insntest.c"), "-o", str(exe)], check=True)

    ref = Ref(prog)
    ref.mu.mem_map(CODE, 0x10000)
    for t, _, _, code in tests:
        ref.mu.mem_write(t, bytes(code) + TAIL)
    (MOD / "image.bin").write_bytes(ref.image)
    d_lo, d_hi = prog.img.section(".data")
    data_region = (d_lo, bytes(ref.image[d_lo - prog.img.base:d_hi - prog.img.base]))
    tmp = BUILD / "tmp"
    tmp.mkdir(exist_ok=True)

    stats = {}
    failures = []
    for t, a, m, _ in tests:
        s = stats.setdefault(m, dict(pass_=0, fail=0, ref_skip=0, native_err=0))
        for _ in range(args.trials):
            regs = [rand_value(rng) if rng.random() < 0.6 else rng.getrandbits(32) for _ in range(8)]
            regs[4] = STACK_LO + STACK_SZ - 0x4000
            stack = bytearray(rand_block(rng, STACK_SZ))
            struct.pack_into("<I", stack, regs[4] - STACK_LO, SENTINEL)
            scratch = bytearray(rand_block(rng, SCRATCH_SZ))
            if a is None:  # x87 register form: ordinary float operands
                struct.pack_into("<3f", scratch, 0, *(rng.uniform(-1e3, 1e3) for _ in range(3)))
            regions = [(STACK_LO, bytes(stack)), (SCRATCH_LO, bytes(scratch)), (TEB, rand_block(rng, 0x1000))]
            xmm = bytes(rng.getrandbits(8) for _ in range(128)) if rng.random() < 0.5 else \
                b"".join(struct.pack("<2d", rng.uniform(-1e3, 1e3), rng.uniform(-1e3, 1e3)) for _ in range(8))
            ref.mu.reg_write(X.UC_X86_REG_EFLAGS, 0x202)  # native starts with all status flags clear
            try:
                want = ref.run(t, regs, xmm, regions)
            except (uc.UcError, RuntimeError):
                s["ref_skip"] += 1
                continue
            got, err = run_native(BUILD / "insntest", t, regs, xmm, regions, data_region, tmp)
            if err:
                s["native_err"] += 1
                failures.append((a, m, err))
                continue
            diffs = [f"{NAMES[k]} {want[0][k]:#x} != {got[0][k]:#x}" for k in range(8) if want[0][k] != got[0][k]]
            diffs += [f"xmm{k} {want[1][k*16:k*16+16].hex()} != {got[1][k*16:k*16+16].hex()}"
                      for k in range(8) if want[1][k * 16:k * 16 + 16] != got[1][k * 16:k * 16 + 16]]
            fl = FLAGS - SCRATCH_LO
            wf, gf = (struct.unpack_from("<I", r[2][1], fl)[0] for r in (want, got))
            mask = FLAG_MASK.get(m, 0xffffffff)
            if (wf ^ gf) & mask:
                diffs.append(f"eflags {wf & mask:#x} != {gf & mask:#x}")
            for k, (w, g) in enumerate(zip(want[2], got[2])):
                if k == 1:  # scratch: ignore the flags slot (masked above)
                    w, g = w[:fl], g[:fl]
                if k == 0:  # stack: ignore the dead copy pushfd left below esp
                    sl = regs[4] - 4 - STACK_LO
                    w, g = w[:sl] + w[sl + 4:], g[:sl] + g[sl + 4:]
                if w != g:
                    first = next(j for j in range(len(w)) if w[j] != g[j])
                    diffs.append(f"{['stack', 'scratch', 'teb', '.data'][k]}+{first:#x}")
            if diffs:
                s["fail"] += 1
                failures.append((a, m, "; ".join(diffs[:3])))
            else:
                s["pass_"] += 1
    total = dict(pass_=0, fail=0, ref_skip=0, native_err=0)
    for m, s in stats.items():
        print(f"  {m:10} {s}")
        for k in total:
            total[k] += s[k]
    print(total)
    seen = set()
    for a, m, why in failures:
        if (a, m) not in seen:
            seen.add((a, m))
            if a is None:
                print(f"  {m}: {why}")
            else:
                i = prog.decode(a)
                print(f"  {a:#x} {i.mnemonic} {i.op_str}: {why}")
    sys.exit(1 if total["fail"] or total["native_err"] else 0)


if __name__ == "__main__":
    main()
