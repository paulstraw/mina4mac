"""Native multithreaded test of locked instructions: hammer recompiled lock xadd / xchg / lock cmpxchg /
lock cmpxchg8b from several host threads and check no update is lost (runtime/atomic_test.c).

Each test function is one real instruction from noita.exe followed by `ret`, lifted by FnLifter.

  uv run tools/atomictest.py [--threads 8] [--iters 200000]
"""
import argparse
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from lift import FnLifter, Program, ROOT  # noqa: E402

BUILD = ROOT / "build"
# name -> instance in noita.exe; runtime/atomic_test.c relies on these operand registers.
INSNS = {
    "xadd": 0x92fcf1,       # lock xadd dword ptr [ecx], eax
    "xchg": 0x84a0ef,       # xchg dword ptr [eax], esi
    "cmpxchg": 0xdfadee,    # lock cmpxchg dword ptr [esi], ecx
    "cmpxchg8b": 0x84a1f5,  # lock cmpxchg8b qword ptr [esi]
}
CODE = 0x0C000000


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--threads", type=int, default=8)
    ap.add_argument("--iters", type=int, default=200000)
    args = ap.parse_args()
    prog = Program()
    out = ['#include "cpu.h"']
    for k, (name, a) in enumerate(INSNS.items()):
        t = CODE + 32 * k
        i = prog.decode(a)
        ret = next(prog.md.disasm(b"\xc3", t + i.size))
        lf = FnLifter(prog, t)
        lf.body_set, lf.callees = set(), set()
        for ins in (i, ret):
            lf.cur = ins
            lf.lift_insn(ins)
        out.append(f"// {i.mnemonic} {i.op_str} (from {a:#x})")
        out.append(f"void T_{name}(CPU *restrict c) {{")
        out.append("  uint32_t eax=c->eax, ecx=c->ecx, edx=c->edx, ebx=c->ebx, esp=c->esp, ebp=c->ebp, esi=c->esi, edi=c->edi;")
        out.append("  uint8_t cf=0, zf=0, sf=0, of=0, pf=0;")
        out.extend(lf.lines)
        out.append("}")
    gen = BUILD / "gen"
    gen.mkdir(parents=True, exist_ok=True)
    (gen / "atomictest.c").write_text("\n".join(out) + "\n")
    exe = BUILD / "atomictest"
    subprocess.run(["clang", "-O2", "-ffp-contract=off", "-fno-strict-aliasing", "-w", "-I", str(ROOT / "runtime"),
                    str(ROOT / "runtime/atomic_test.c"), str(gen / "atomictest.c"), "-o", str(exe)], check=True)
    sys.exit(subprocess.run([str(exe), str(args.threads), str(args.iters)]).returncode)


if __name__ == "__main__":
    main()
