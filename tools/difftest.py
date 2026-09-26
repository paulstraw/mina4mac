"""Differential testing: Unicorn (reference x86) vs recompiled native code, same random inputs.

  uv run tools/difftest.py [--funcs N] [--trials T] [--seed S]
"""
import argparse
import os
import pickle
import random
import struct
import subprocess
import sys
from pathlib import Path

import unicorn as uc
from unicorn import x86_const as X

sys.path.insert(0, str(Path(__file__).parent))
from lift import FnLifter, Program, ROOT  # noqa: E402

BUILD = ROOT / "build"
GEN = BUILD / "gen"
STACK_LO, STACK_SZ = 0x08000000, 0x10000
SCRATCH_LO, SCRATCH_SZ = 0x10000000, 0x40000
TEB = 0x7FFD0000
GDT = 0x7FFE0000
SENTINEL = 0x0BADF000
GPRS = [X.UC_X86_REG_EAX, X.UC_X86_REG_ECX, X.UC_X86_REG_EDX, X.UC_X86_REG_EBX,
        X.UC_X86_REG_ESP, X.UC_X86_REG_EBP, X.UC_X86_REG_ESI, X.UC_X86_REG_EDI]
NAMES = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi", "fs_base"]


def closure(ok, roots):
    seen, work = set(), list(roots)
    while work:
        a = work.pop()
        if a in seen:
            continue
        seen.add(a)
        if a not in ok:
            return None
        work.extend(ok[a]["callees"])
    return seen


def pick_candidates(ok, n, rng, allow_indirect=False):
    good = []
    for a, info in ok.items():
        if info["indirect"] and not allow_indirect:
            continue
        cl = closure(ok, [a])
        if cl is None or len(cl) > 400:
            continue
        if any(ok[b]["imports"] if allow_indirect else ok[b]["indirect"] for b in cl):
            continue
        good.append((a, cl))
    rng.shuffle(good)
    return good[:n], len(good)


def rand_value(rng):
    r = rng.random()
    if r < 0.45:
        return SCRATCH_LO + rng.randrange(0, SCRATCH_SZ - 256) & ~3
    if r < 0.7:
        return rng.randrange(0, 16)
    return struct.unpack("<I", struct.pack("<f", rng.uniform(-100, 100)))[0]


def rand_block(rng, size):
    return b"".join(struct.pack("<I", rand_value(rng)) for _ in range(size // 4))


def build_native(prog, funcs):
    GEN.mkdir(parents=True, exist_ok=True)
    out = ['#include "cpu.h"']
    out += [f"void F_{a:08x}(CPU *restrict c);" for a in sorted(funcs)]
    for a in sorted(funcs):
        out.append(FnLifter(prog, a).lift())
    out.append("typedef struct { uint32_t addr; GuestFn fn; } FnEntry;")
    out.append("const FnEntry FN_TABLE[] = {" + ",".join(f"{{{a:#x}u,F_{a:08x}}}" for a in sorted(funcs)) + "};")
    out.append(f"const int FN_COUNT = {len(funcs)};")
    (GEN / "code.c").write_text("\n".join(out))
    exe = BUILD / "harness"
    subprocess.run(["clang", "-O2", "-ffp-contract=off", "-fno-strict-aliasing", "-w",
                    "-I", str(ROOT / "runtime"), str(ROOT / "runtime/harness.c"), str(GEN / "code.c"),
                    "-o", str(exe)], check=True)
    return exe


class Ref:
    """Unicorn machine with the image mapped once; reset writable state per trial."""

    def __init__(self, prog):
        self.img = prog.img
        self.mu = uc.Uc(uc.UC_ARCH_X86, uc.UC_MODE_32)
        mem = bytes(self.img.mem)
        self.image = mem
        lo = self.img.base
        hi = (lo + len(mem) + 0xfff) & ~0xfff
        self.mu.mem_map(lo, hi - lo)
        self.mu.mem_write(lo, mem)
        self.d_lo, self.d_hi = self.img.section(".data")
        self.mu.mem_map(STACK_LO, STACK_SZ)
        self.mu.mem_map(SCRATCH_LO, SCRATCH_SZ)
        self.mu.mem_map(TEB, 0x1000)
        self.mu.mem_map(SENTINEL, 0x1000)
        self.mu.mem_map(GDT, 0x1000)
        # GDT with a flat data segment for fs based at TEB.
        def desc(base, limit, access, flags):
            return (limit & 0xffff) | ((base & 0xffffff) << 16) | (access << 40) | (((limit >> 16) & 0xf) << 48) | (flags << 52) | (((base >> 24) & 0xff) << 56)
        gdt = [0, desc(0, 0xfffff, 0x9a, 0xc), desc(0, 0xfffff, 0x92, 0xc), desc(TEB, 0xfff, 0x92, 0x4)]
        self.mu.mem_write(GDT, b"".join(struct.pack("<Q", g) for g in gdt))
        self.mu.reg_write(X.UC_X86_REG_GDTR, (0, GDT, len(gdt) * 8 - 1, 0))
        self.mu.reg_write(X.UC_X86_REG_CS, 1 << 3)
        for seg in (X.UC_X86_REG_DS, X.UC_X86_REG_ES, X.UC_X86_REG_SS):
            self.mu.reg_write(seg, 2 << 3)
        self.mu.reg_write(X.UC_X86_REG_FS, 3 << 3)
        cr0 = self.mu.reg_read(X.UC_X86_REG_CR0)
        self.mu.reg_write(X.UC_X86_REG_CR0, (cr0 & ~4) | 2)
        self.mu.reg_write(X.UC_X86_REG_CR4, self.mu.reg_read(X.UC_X86_REG_CR4) | (3 << 9))

    def run(self, func, regs, xmm, regions, limit=5_000_000):
        mu = self.mu
        mu.mem_write(self.d_lo, self.image[self.d_lo - self.img.base:self.d_hi - self.img.base])
        for addr, data in regions:
            mu.mem_write(addr, data)
        for r, v in zip(GPRS, regs):
            mu.reg_write(r, v)
        for k in range(8):
            mu.reg_write(X.UC_X86_REG_XMM0 + k, int.from_bytes(xmm[k * 16:(k + 1) * 16], "little"))
        mu.reg_write(X.UC_X86_REG_FPSW, 0)
        mu.reg_write(X.UC_X86_REG_FPTAG, 0xffff)
        for k in range(8):
            mu.reg_write(X.UC_X86_REG_FP0 + k, (0, 0))
        mu.reg_write(X.UC_X86_REG_FPCW, 0x27F)  # Windows default: 53-bit precision, round-to-nearest
        mu.emu_start(func, SENTINEL, count=limit)
        if mu.reg_read(X.UC_X86_REG_EIP) != SENTINEL:
            raise RuntimeError("instruction limit")
        out_regs = [mu.reg_read(r) for r in GPRS] + [TEB]
        out_xmm = b"".join(mu.reg_read(X.UC_X86_REG_XMM0 + k).to_bytes(16, "little") for k in range(8))
        out_mem = [bytes(mu.mem_read(a, len(d))) for a, d in regions] + [bytes(mu.mem_read(self.d_lo, self.d_hi - self.d_lo))]
        return out_regs, out_xmm, out_mem


def run_native(exe, func, regs, xmm, regions, data_region, tmp):
    snap = tmp / "snap.bin"
    res = tmp / "res.bin"
    all_regions = regions + [data_region]
    with open(snap, "wb") as f:
        f.write(struct.pack("<I", func))
        f.write(struct.pack("<9I", *regs, TEB))
        f.write(xmm)
        f.write(struct.pack("<I", len(all_regions)))
        for a, d in all_regions:
            f.write(struct.pack("<II", a, len(d)))
            f.write(d)
    p = subprocess.run([str(exe), str(snap), str(res), str(BUILD / "image.bin"), "400000"], capture_output=True, timeout=10)
    if p.returncode != 0:
        return None, p.stderr.decode().strip() or f"exit {p.returncode}"
    b = res.read_bytes()
    out_regs = list(struct.unpack_from("<9I", b, 0))
    out_xmm = b[36:36 + 128]
    off = 164
    out_mem = []
    for a, d in all_regions:
        out_mem.append(b[off:off + len(d)])
        off += len(d)
    return (out_regs, out_xmm, out_mem), None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--funcs", type=int, default=200)
    ap.add_argument("--trials", type=int, default=4)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--only", type=lambda s: int(s, 16))
    ap.add_argument("--x87", action="store_true", help="only functions whose own body uses x87")
    ap.add_argument("--all", action="store_true", help="use the whole-program build (indirect calls allowed)")
    args = ap.parse_args()
    rng = random.Random(args.seed)
    prog = Program()
    ok = pickle.load(open(BUILD / "survey.pkl", "rb"))["ok"]
    if args.only:
        cands, total = [(args.only, closure(ok, [args.only]))], 1
    else:
        if args.x87:
            ok_x87 = {a: v for a, v in ok.items()
                      if any(prog.insns[x][1].startswith("f") for x in prog.function_body(a))}
            pool, _ = pick_candidates(ok, 10**9, rng, args.all)
            pool = [(a, cl) for a, cl in pool if a in ok_x87]
            cands, total = pool[:args.funcs], len(pool)
        else:
            cands, total = pick_candidates(ok, args.funcs, rng, args.all)
    print(f"{total:,} functions have self-contained call trees; testing {len(cands)}")
    funcs = set().union(*(cl for _, cl in cands))
    exe = BUILD / "harness_all" if args.all else build_native(prog, funcs)
    ref = Ref(prog)
    assert prog.img.base == 0x400000
    (BUILD / "image.bin").write_bytes(ref.image)
    tmp = BUILD / "tmp"
    tmp.mkdir(exist_ok=True)
    d_lo, d_hi = prog.img.section(".data")
    data_region = (d_lo, bytes(ref.image[d_lo - prog.img.base:d_hi - prog.img.base]))
    stats = dict(pass_=0, fail=0, ref_skip=0, native_err=0)
    failures = []
    for a, cl in cands:
        for t in range(args.trials):
            regs = [rand_value(rng) for _ in range(8)]
            regs[4] = STACK_LO + STACK_SZ - 0x4000  # esp
            stack = bytearray(rand_block(rng, STACK_SZ))
            struct.pack_into("<I", stack, regs[4] - STACK_LO, SENTINEL)  # return address
            teb = bytearray(rand_block(rng, 0x1000))
            struct.pack_into("<I", teb, 0, 0xFFFFFFFF)
            regions = [(STACK_LO, bytes(stack)), (SCRATCH_LO, rand_block(rng, SCRATCH_SZ)), (TEB, bytes(teb))]
            xmm = b"".join(struct.pack("<4f", *[rng.uniform(-50, 50) for _ in range(4)]) for _ in range(8))
            try:
                want = ref.run(a, regs, xmm, regions)
            except (uc.UcError, RuntimeError):
                stats["ref_skip"] += 1
                continue
            got, err = run_native(exe, a, regs, xmm, regions, data_region, tmp)
            if err:
                stats["native_err"] += 1
                failures.append((a, t, err))
                continue
            diffs = []
            for k in range(8):
                if want[0][k] != got[0][k]:
                    diffs.append(f"{NAMES[k]} {want[0][k]:#x} != {got[0][k]:#x}")
            if want[1] != got[1]:
                for k in range(8):
                    if want[1][k * 16:k * 16 + 16] != got[1][k * 16:k * 16 + 16]:
                        diffs.append(f"xmm{k}")
            names = ["stack", "scratch", "teb", ".data"]
            for k, (w, g) in enumerate(zip(want[2], got[2])):
                if w != g:
                    first = next(j for j in range(len(w)) if w[j] != g[j])
                    diffs.append(f"{names[k]}+{first:#x}")
            if diffs:
                stats["fail"] += 1
                failures.append((a, t, "; ".join(diffs[:4])))
            else:
                stats["pass_"] += 1
    print(stats)
    seen = set()
    for a, t, why in failures:
        if a in seen:
            continue
        seen.add(a)
        print(f"  {a:#x} trial {t}: {why}")
    pickle.dump(failures, open(BUILD / "difftest_failures.pkl", "wb"))


if __name__ == "__main__":
    main()
