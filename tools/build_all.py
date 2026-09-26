"""Lift every function of the given modules (default: noita and msvcp120, which the launcher needs) into chunked C files, compile them in
parallel, link the test harness (build/harness_all), the microbenchmark (build/bench <image.bin>) and the
launcher (build/noitamac, which also links the SDL2 bridge from tools/gen_sdl.py and the host's SDL2, and the LuaJIT
bridge with LuaJIT built from third_party/luajit into build/luajit/).

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
import gen_sdl  # noqa: E402

GEN = ROOT / "build/gen_all"
LAUNCHER = ("main.c", "heap.c", "proc.c", "msvcr120.c", "msvcr120_stdio.c", "msvcr120_string.c", "kernel32.c", "kernel32_file.c", "undname.c", "msvcr120_concrt.c", "sync.c", "hle.c", "msvcr120_math.c", "shlwapi.c", "shell32.c", "galaxy.c", "wininet.c", "fmod_stub.c", "opengl32.c", "thread.c", "sdl2_stdlib.c")  # runtime files only the launcher links
SDL = ("sdl2.c",)  # runtime files of the SDL2 bridge (launcher only; compiled with the host SDL2's flags)
LUA = ("lua51.c",)  # the LuaJIT bridge (launcher only; compiled with LUA_CFLAGS, linked with LUAJIT_LIB)
LUAJIT_SRC = ROOT / "third_party/luajit"
LUAJIT_BUILD = ROOT / "build/luajit"  # a copy of the submodule, built in place, so the submodule stays clean
LUAJIT_LIB = LUAJIT_BUILD / "src/libluajit.a"
# Lua errors unwind through the bridge (external unwinding), which restores guest state in cleanups.
LUA_CFLAGS = ["-I", str(LUAJIT_SRC / "src"), "-fexceptions"]
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


def build_luajit():
    """Build the static host LuaJIT 2.1 (ARM64, JIT enabled) from the submodule; make is incremental."""
    if not (LUAJIT_SRC / "src/lua.h").exists():
        sys.exit("third_party/luajit is missing: git submodule update --init")
    LUAJIT_BUILD.mkdir(parents=True, exist_ok=True)
    subprocess.run(["rsync", "-a", "--exclude", ".git", f"{LUAJIT_SRC}/", f"{LUAJIT_BUILD}/"], check=True)
    subprocess.run(["make", "-C", str(LUAJIT_BUILD), f"-j{os.cpu_count()}", "BUILDMODE=static", "amalg",
                    "MACOSX_DEPLOYMENT_TARGET=14.0"], check=True, capture_output=True)
    return LUAJIT_LIB


def _sdl_config(flag):
    return subprocess.run(["sdl2-config", flag], capture_output=True, text=True, check=True).stdout.split()


def _cc(src):
    # Chunk objects sit next to their sources; runtime objects go in GEN.
    obj = (src.parent if src.name.startswith("chunk") else GEN) / (src.stem + ".o")
    sdl = _sdl_config("--cflags") if src.stem.startswith("sdl2") and src.stem != "sdl2_stdlib" else []
    lua = LUA_CFLAGS if src.name in LUA else []
    subprocess.run(["clang", "-c", "-O2", "-ffp-contract=off", "-fno-strict-aliasing", "-w", *sdl, *lua,
                    "-I", str(ROOT / "runtime"), "-I", str(GEN), str(src), "-o", str(obj)], check=True)
    return obj


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("modules", nargs="*", choices=MODULES)
    modules = ap.parse_args().modules or ["noita", "msvcp120"]
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
    gen_sdl.generate()
    build_luajit()
    launcher = [*LAUNCHER, *SDL, *LUA, "sdl2_gen.c"]
    srcs = chunk_srcs + [GEN / "table.c", GEN / "sdl2_gen.c"] + [ROOT / "runtime" / n for n in ("rt.c", "harness.c", "bench.c", *LAUNCHER, *SDL, *LUA)]
    with mp.Pool(os.cpu_count()) as pool:
        objs = pool.map(_cc, srcs)
    t2 = time.time()
    only = {"harness", "bench"} | {Path(n).stem for n in launcher}
    common = [str(o) for o in objs if o.stem not in only]
    exe = ROOT / "build/harness_all"
    subprocess.run(["clang", *common, str(GEN / "harness.o"), "-o", str(exe)], check=True)
    subprocess.run(["clang", *common, str(GEN / "bench.o"), "-o", str(ROOT / "build/bench")], check=True)
    subprocess.run(["clang", *common, *(str(GEN / (Path(n).stem + ".o")) for n in launcher),
                    str(LUAJIT_LIB), *_sdl_config("--libs"), "-framework", "OpenGL", "-o", str(ROOT / "build/noitamac")], check=True)
    t3 = time.time()
    src_mb = sum(s.stat().st_size for s in chunk_srcs) / 1e6
    print(f"functions {len(starts):,} (stubbed {failed:,})")
    print(f"lift {t1 - t0:.0f}s, compile {t2 - t1:.0f}s, link {t3 - t2:.0f}s; C source {src_mb:.0f} MB; "
          f"binary {exe.stat().st_size / 1e6:.1f} MB")


if __name__ == "__main__":
    main()
