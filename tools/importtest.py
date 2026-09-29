"""Import thunk test (runtime/import_test.c): bind noita.exe's real IAT to host thunks, then run lifted
`call [slot]` and `jmp [slot]` instructions against fake host implementations, and check that an
unimplemented import exits naming dll!name and the guest return address.

  uv run tools/importtest.py
"""
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from lift import FnLifter, Program, ROOT  # noqa: E402
from pe import build_dir  # noqa: E402

BUILD = ROOT / "build"
MOD = build_dir("noita")
CODE = 0x0C000000
# test name -> (instruction bytes before the slot address, import). runtime/import_test.c relies on these.
TESTS = {
    "call": (b"\xff\x15", ("KERNEL32.dll", "GetCurrentProcessId")),      # call dword ptr [slot]; ret
    "jmp": (b"\xff\x25", ("KERNEL32.dll", "QueryPerformanceCounter")),   # jmp dword ptr [slot]
    "unimpl": (b"\xff\x15", ("KERNEL32.dll", "GetCurrentThreadId")),     # call dword ptr [slot]; ret
}


def main():
    prog = Program()
    slots = {v: k for k, v in prog.img.imports.items()}
    out = ['#include "rt.h"', "const FnEntry FN_TABLE[1];", "const int FN_COUNT = 0;",
           f"const int N_IMPORTS = {len(prog.img.imports)};"]
    for k, (name, (prefix, imp)) in enumerate(TESTS.items()):
        t = CODE + 32 * k
        code = prefix + slots[imp].to_bytes(4, "little") + b"\xc3"
        insns = list(prog.md.disasm(code, t))
        if name == "jmp":
            insns = insns[:1]
        lf = FnLifter(prog, t)
        lf.body_set, lf.callees = set(), set()
        for ins in insns:
            lf.cur = ins
            lf.lift_insn(ins)
        assert lf.import_slots == {slots[imp]}
        out.append(f"// {'; '.join(f'{i.mnemonic} {i.op_str}' for i in insns)}  ({imp[0]}!{imp[1]})")
        out.append(f'const char *{name.upper()}_DLL = "{imp[0]}", *{name.upper()}_NAME = "{imp[1]}";')
        out.append(f"const uint32_t {name.upper()}_SLOT = {slots[imp]:#x}u, {name.upper()}_RET = {t + insns[0].size:#x}u;")
        out.append(f"void T_{name}(CPU *restrict c) {{")
        out.append("  uint32_t eax=c->eax, ecx=c->ecx, edx=c->edx, ebx=c->ebx, esp=c->esp, ebp=c->ebp, esi=c->esi, edi=c->edi;")
        out.append("  uint8_t cf=0, zf=0, sf=0, of=0, pf=0;")
        lf.finish_syncs(mode="full")
        out.extend(lf.lines)
        out.append("}")
    gen = MOD / "testgen"
    gen.mkdir(parents=True, exist_ok=True)
    (gen / "importtest.c").write_text("\n".join(out) + "\n")
    image = MOD / "image.bin"
    image.write_bytes(bytes(prog.img.mem))
    exe = BUILD / "importtest"
    subprocess.run(["clang", "-O2", "-ffp-contract=off", "-fno-strict-aliasing", "-w", "-I", str(ROOT / "runtime"),
                    str(ROOT / "runtime/import_test.c"), str(ROOT / "runtime/rt.c"), str(gen / "importtest.c"),
                    "-o", str(exe)], check=True)
    ok = subprocess.run([str(exe), str(image)]).returncode == 0
    p = subprocess.run([str(exe), str(image), "unimpl"], capture_output=True, text=True)
    want = f"unimplemented import KERNEL32.dll!GetCurrentThreadId (called from {CODE + 64 + 6:#x})"
    got = p.stderr.strip()
    unimpl_ok = p.returncode == 4 and got == want
    print(f"unimpl     {'ok  ' if unimpl_ok else 'FAIL'} exit {p.returncode}: {got!r}")
    print("importtest:", "ok" if ok and unimpl_ok else "FAIL")
    sys.exit(0 if ok and unimpl_ok else 1)


if __name__ == "__main__":
    main()
