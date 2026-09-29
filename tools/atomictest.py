"""Native multithreaded test of locked instructions and memory ordering (runtime/atomic_test.c).

Locked: hammer recompiled lock xadd / xchg / lock cmpxchg / lock cmpxchg8b from several host threads
and check no update is lost. Ordering (tools/memorder.py): real game instructions lifted twice, with
x86 ordering (acquire/release, `_ord`) and without (`_plain`):
  - the job wait `mov eax, [edi+0x1c]` (0x726a51) in a message-passing test against a lock xadd,
  - a spinlock released with a lifted plain store `mov [esi+0x18], 0` (0x84a1bb),
  - the spin-wait loop at 0x84aba0, which clang hoists into `b .` when lifted plain.

Each test function is a run of real instructions from noita.exe followed by `ret`, lifted by FnLifter.

  uv run tools/atomictest.py [--threads 8] [--iters 200000] [--rounds 200000]
"""
import argparse
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from lift import FnLifter, Program, ROOT  # noqa: E402
from pe import build_dir  # noqa: E402

BUILD = ROOT / "build"
# name -> instruction run (first, last) in noita.exe; runtime/atomic_test.c relies on these operand registers.
INSNS = {
    "xadd": (0x92fcf1, 0x92fcf1),       # lock xadd dword ptr [ecx], eax
    "xchg": (0x84a0ef, 0x84a0ef),       # xchg dword ptr [eax], esi
    "cmpxchg": (0xdfadee, 0xdfadee),    # lock cmpxchg dword ptr [esi], ecx
    "cmpxchg8b": (0x84a1f5, 0x84a1f5),  # lock cmpxchg8b qword ptr [esi]
}
ORDERED = {
    "poll": (0x726a51, 0x726a51),    # mov eax, dword ptr [edi + 0x1c]    (job pending count)
    "unlock": (0x84a1bb, 0x84a1bb),  # mov dword ptr [esi + 0x18], 0
    "spin": (0x84aba0, 0x84aba8),    # mov eax, [0x12056d0]; cmp eax, 2; jne 0x84aba0
}


def lift_run(prog, name, first, last, tso):
    prog.tso_mode = tso
    addrs, a = [], first
    while a <= last:
        addrs.append(a)
        a += prog.insns[a][0]
    lf = FnLifter(prog, first)
    lf.body_set, lf.callees = set(addrs), set()
    out = [f"// {name}: {first:#x}..{last:#x}, MINA4MAC_TSO={tso}",
           f"void T_{name}(CPU *restrict c) {{",
           "  uint32_t eax=c->eax, ecx=c->ecx, edx=c->edx, ebx=c->ebx, esp=c->esp, ebp=c->ebp, esi=c->esi, edi=c->edi;",
           "  uint8_t cf=0, zf=0, sf=0, of=0, pf=0;"]
    ret = next(prog.md.disasm(b"\xc3", a))
    for i in [prog.decode(x) for x in addrs] + [ret]:
        lf.cur = i
        lf.lines.append(f"L_{i.address:08x}: ;  // {i.mnemonic} {i.op_str}")
        lf.lift_insn(i)
    lf.finish_syncs(mode="full")
    out.extend(lf.lines)
    out.append("}")
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--threads", type=int, default=8)
    ap.add_argument("--iters", type=int, default=200000)
    ap.add_argument("--rounds", type=int, default=200000)
    args = ap.parse_args()
    prog = Program()
    out = ['#include "rt.h"', "const FnEntry FN_TABLE[1];", "const int FN_COUNT = 0;"]
    for name, (a, b) in INSNS.items():
        out += lift_run(prog, name, a, b, "auto")
    for name, (a, b) in ORDERED.items():
        prog.tso_mode = "auto"
        assert all(prog.is_ordered(x) for x in (a, b)), f"{name}: memorder.py no longer orders {a:#x}"
        out += lift_run(prog, name + "_ord", a, b, "all")
        out += lift_run(prog, name + "_plain", a, b, "off")
    gen = build_dir("noita") / "testgen"
    gen.mkdir(parents=True, exist_ok=True)
    (gen / "atomictest.c").write_text("\n".join(out) + "\n")
    exe = BUILD / "atomictest"
    subprocess.run(["clang", "-O2", "-ffp-contract=off", "-fno-strict-aliasing", "-w", "-I", str(ROOT / "runtime"),
                    str(ROOT / "runtime/atomic_test.c"), str(ROOT / "runtime/rt.c"), str(gen / "atomictest.c"), "-o", str(exe)], check=True)
    sys.exit(subprocess.run([str(exe), str(args.threads), str(args.iters), str(args.rounds)]).returncode)


if __name__ == "__main__":
    main()
