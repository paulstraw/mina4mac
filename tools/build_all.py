"""Lift every function of the given modules (default: noita) into chunked C files, compile them in
parallel, link the test harness (build/harness_all) and the microbenchmark (build/bench <image.bin>).

Per-module chunks go in build/<module>/gen/; the combined decls.h, FN_TABLE and runtime objects go in
build/gen_all/. Functions the lifter can't handle yet become stubs that report guest_unimpl.

  uv run tools/build_all.py [module ...]
"""
import argparse
import multiprocessing as mp
import os
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from lift import FnLifter, Program, ROOT, Unsupported  # noqa: E402
from pe import MODULES, build_dir  # noqa: E402

GEN = ROOT / "build/gen_all"
CHUNKS = 128
_prog = None


def _init(module):
    global _prog
    _prog = Program(module)


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
    (build_dir(_prog.module) / "gen" / f"chunk{k:03}.c").write_text("\n".join(out))
    return failed


def _cc(src):
    # Chunk objects sit next to their sources; runtime objects go in GEN.
    obj = (src.parent if src.name.startswith("chunk") else GEN) / (src.stem + ".o")
    subprocess.run(["clang", "-c", "-O2", "-ffp-contract=off", "-fno-strict-aliasing", "-w",
                    "-I", str(ROOT / "runtime"), "-I", str(GEN), str(src), "-o", str(obj)], check=True)
    return obj


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("modules", nargs="*", choices=MODULES)
    modules = ap.parse_args().modules or ["noita"]
    GEN.mkdir(parents=True, exist_ok=True)
    starts_by_mod = {}
    for m in modules:
        prog = Program(m)
        starts_by_mod[m] = sorted(a for a in prog.all_starts | prog.direct_calls if prog.t_lo <= a < prog.t_hi)
    starts = sorted(a for s in starts_by_mod.values() for a in s)
    assert len(starts) == len(set(starts)), "modules overlap"
    (GEN / "decls.h").write_text("\n".join(f"void F_{a:08x}(CPU *restrict c);" for a in starts))
    t0 = time.time()
    failed = 0
    chunk_srcs = []
    for m, ms in starts_by_mod.items():
        mgen = build_dir(m) / "gen"
        mgen.mkdir(parents=True, exist_ok=True)
        for old in mgen.glob("chunk*"):
            old.unlink()
        chunks = [(k, ms[k::CHUNKS]) for k in range(CHUNKS)]
        with mp.Pool(os.cpu_count(), initializer=_init, initargs=(m,)) as pool:
            failed += sum(pool.map(_lift_chunk, chunks))
        chunk_srcs += sorted(mgen.glob("chunk*.c"))
    t1 = time.time()
    table = ['#include "rt.h"', '#include "decls.h"',
             "const FnEntry FN_TABLE[] = {" + ",".join(f"{{{a:#x}u,F_{a:08x}}}" for a in starts) + "};",
             f"const int FN_COUNT = {len(starts)};"]
    (GEN / "table.c").write_text("\n".join(table))
    srcs = chunk_srcs + [GEN / "table.c"] + [ROOT / "runtime" / n for n in ("rt.c", "harness.c", "bench.c")]
    with mp.Pool(os.cpu_count()) as pool:
        objs = pool.map(_cc, srcs)
    t2 = time.time()
    common = [str(o) for o in objs if o.stem not in ("harness", "bench")]
    exe = ROOT / "build/harness_all"
    subprocess.run(["clang", *common, str(GEN / "harness.o"), "-o", str(exe)], check=True)
    subprocess.run(["clang", *common, str(GEN / "bench.o"), "-o", str(ROOT / "build/bench")], check=True)
    t3 = time.time()
    src_mb = sum(s.stat().st_size for s in chunk_srcs) / 1e6
    print(f"functions {len(starts):,} (stubbed {failed:,})")
    print(f"lift {t1 - t0:.0f}s, compile {t2 - t1:.0f}s, link {t3 - t2:.0f}s; C source {src_mb:.0f} MB; "
          f"binary {exe.stat().st_size / 1e6:.1f} MB")


if __name__ == "__main__":
    main()
