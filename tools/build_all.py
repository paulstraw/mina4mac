"""Lift every function into chunked C files, compile them in parallel, link the test harness
(build/harness_all) and the microbenchmark (build/bench <image.bin>).

Functions the lifter can't handle yet become stubs that report guest_unimpl.
"""
import multiprocessing as mp
import os
import pickle
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from lift import FnLifter, Program, ROOT, Unsupported  # noqa: E402

GEN = ROOT / "build/gen_all"
CHUNKS = 128
_prog = None


def _init():
    global _prog
    _prog = Program()


def _lift_chunk(args):
    k, addrs = args
    out = ['#include "cpu.h"', '#include "decls.h"']
    failed = 0
    for a in addrs:
        try:
            out.append(FnLifter(_prog, a).lift())
        except Unsupported as e:
            failed += 1
            msg = str(e).replace('"', "'")[:80]
            out.append(f'void F_{a:08x}(CPU *restrict c) {{ guest_unimpl(c, {a:#x}u, "{msg}"); }}')
    (GEN / f"chunk{k:03}.c").write_text("\n".join(out))
    return failed


def _cc(src):
    obj = GEN / (src.stem + ".o")
    subprocess.run(["clang", "-c", "-O2", "-ffp-contract=off", "-fno-strict-aliasing", "-w",
                    "-I", str(ROOT / "runtime"), "-I", str(GEN), str(src), "-o", str(obj)], check=True)
    return obj


def main():
    prog = Program()
    starts = sorted(a for a in prog.all_starts | prog.direct_calls if prog.t_lo <= a < prog.t_hi)
    GEN.mkdir(parents=True, exist_ok=True)
    (GEN / "decls.h").write_text("\n".join(f"void F_{a:08x}(CPU *restrict c);" for a in starts))
    chunks = [(k, starts[k::CHUNKS]) for k in range(CHUNKS)]
    t0 = time.time()
    with mp.Pool(os.cpu_count(), initializer=_init) as pool:
        failed = sum(pool.map(_lift_chunk, chunks))
    t1 = time.time()
    table = ['#include "rt.h"', '#include "decls.h"',
             "const FnEntry FN_TABLE[] = {" + ",".join(f"{{{a:#x}u,F_{a:08x}}}" for a in starts) + "};",
             f"const int FN_COUNT = {len(starts)};"]
    (GEN / "table.c").write_text("\n".join(table))
    mains = [ROOT / "runtime/harness.c", ROOT / "runtime/bench.c"]
    srcs = sorted(GEN.glob("chunk*.c")) + [GEN / "table.c", ROOT / "runtime/rt.c"] + mains
    with mp.Pool(os.cpu_count()) as pool:
        objs = pool.map(_cc, srcs)
    t2 = time.time()
    common = [str(o) for o in objs if o.stem not in ("harness", "bench")]
    exe = ROOT / "build/harness_all"
    subprocess.run(["clang", *common, str(GEN / "harness.o"), "-o", str(exe)], check=True)
    subprocess.run(["clang", *common, str(GEN / "bench.o"), "-o", str(ROOT / "build/bench")], check=True)
    t3 = time.time()
    src_mb = sum(s.stat().st_size for s in srcs) / 1e6
    print(f"functions {len(starts):,} (stubbed {failed:,})")
    print(f"lift {t1 - t0:.0f}s, compile {t2 - t1:.0f}s, link {t3 - t2:.0f}s; C source {src_mb:.0f} MB; "
          f"binary {exe.stat().st_size / 1e6:.1f} MB")


if __name__ == "__main__":
    main()
