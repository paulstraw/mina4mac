"""Lift every function of the given modules (default: noita and msvcp120, which the launcher needs) into chunked C files, compile them in
parallel, link the test harness (build/harness_all), the microbenchmark (build/bench <image.bin>) and the
launcher (build/mina4mac, which also links the SDL2 bridge from tools/gen_sdl.py and the host's SDL2, the OpenGL
bridge from tools/gen_gl.py, and the LuaJIT bridge with LuaJIT built from third_party/luajit into build/luajit/).

Per-module chunks go in build/<module>/gen/; the combined decls.h, FN_TABLE and runtime objects go in
build/gen_all/. Functions the lifter can't handle yet become stubs that report guest_unimpl.

  uv run tools/build_all.py [module ...]
  uv run tools/build_all.py --pgo gen    instrumented build (IR PGO): runs add to build/pgo/raw/*.profraw
  uv run tools/build_all.py --pgo use    optimize with build/pgo/mina4mac.profdata (tools/pgo.sh makes both)
  uv run tools/build_all.py --order      link with the order file build/pgo/mina4mac.order (hot functions first)
  uv run tools/build_all.py --opt O3     optimization level (default O2)
  uv run tools/build_all.py --lto off    no ThinLTO (default thin: cross-chunk inlining, cache in build/lto_cache/)
  uv run tools/build_all.py --chunking contiguous   chunks of adjacent functions (default round-robin)
  uv run tools/build_all.py --icprof     count indirect calls per site (MINA4MAC_ICPROF=<file>; tools/icache.py)
                                         and register syncs (<file>.sync)
  uv run tools/build_all.py --ic none    no inline caches (default: the sites in tools/icache_sites.txt)
  uv run tools/build_all.py --sync full|dirty|check   register sync around calls (default live: tools/regsum.py summaries)
  uv run tools/build_all.py --slots on|check   stack slots in C locals (tools/slots.py; default off: no measured gain);
                                         check verifies each slot access's address and value at run time (exit 13)
  MINA4MAC_VCALL=unknown uv run tools/build_all.py    indirect calls preserve no registers (default abi: ebx/esi/edi/ebp)

Profiles come from the user's own game and stay in build/ (never commit them).
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
import regsum  # noqa: E402
import slots  # noqa: E402
from pe import MODULES, build_dir  # noqa: E402
import gen_gl  # noqa: E402
import gen_sdl  # noqa: E402

GEN = ROOT / "build/gen_all"
LAUNCHER = ("main.c", "heap.c", "proc.c", "msvcr120.c", "msvcr120_stdio.c", "msvcr120_string.c", "kernel32.c", "kernel32_file.c", "undname.c", "msvcr120_concrt.c", "sync.c", "hle.c", "msvcr120_math.c", "shlwapi.c", "shell32.c", "galaxy.c", "wininet.c", "ws2_32.c", "user32.c", "fmod.c", "fmod_stub.c", "opengl32.c", "thread.c", "sdl2_stdlib.c", "joblog.c", "sched.c")  # runtime files only the launcher links
SDL = ("sdl2.c",)  # runtime files of the SDL2 bridge (launcher only; compiled with the host SDL2's flags)
LUA = ("lua51.c",)  # the LuaJIT bridge (launcher only; compiled with LUA_CFLAGS, linked with LUAJIT_LIB)
LUAJIT_SRC = ROOT / "third_party/luajit"
LUAJIT_BUILD = ROOT / "build/luajit"  # a copy of the submodule, built in place, so the submodule stays clean
LUAJIT_LIB = LUAJIT_BUILD / "src/libluajit.a"
# Lua errors unwind through the bridge (external unwinding), which restores guest state in cleanups.
LUA_CFLAGS = ["-I", str(LUAJIT_SRC / "src"), "-fexceptions"]
CHUNKS = 128
PGO_DIR = ROOT / "build/pgo"
PGO_RAW = PGO_DIR / "raw"  # the instrumented binary writes default_<signature>.profraw here, merging runs
PGO_DATA = PGO_DIR / "mina4mac.profdata"
PGO_ORDER = PGO_DIR / "mina4mac.order"
LTO_CACHE = ROOT / "build/lto_cache"
IC_SITES = ROOT / "tools/icache_sites.txt"
_prog = None


def _init(module, icprof, ic_targets, sync="live", summaries=None, slot_plans=None, slot_mode="on"):
    global _prog
    _prog = Program(module)
    _prog.ic_profile, _prog.ic_targets, _prog.sync_mode, _prog.slot_mode = icprof, ic_targets, sync, slot_mode
    _prog.summaries = summaries or {}
    _prog.slot_plans = slot_plans or {}


def _facts_chunk(addrs):
    """tools/regsum.py facts for these functions (unliftable ones are left out: callers assume nothing)."""
    out = {}
    for a in addrs:
        try:
            out[a] = FnLifter(_prog, a).lift(facts=True)[1]
        except Unsupported:
            pass
    return out


def _module_starts(prog):
    return sorted(a for a in prog.all_starts | prog.direct_calls if prog.t_lo <= a < prog.t_hi)


def regsum_for(module, with_slots=False):
    """Register-preservation summaries for `module` (tools/regsum.py) and, with_slots, its stack-slot plans
    (tools/slots.py), from the cache or computed and cached."""
    s, plans = regsum.load(module), regsum.load_slots(module)
    if s is None or (with_slots and plans is None):
        starts = _module_starts(Program(module))
        with mp.Pool(os.cpu_count(), initializer=_init, initargs=(module, False, {}, "full")) as pool:
            facts = {}
            for f in pool.imap_unordered(_facts_chunk, [starts[k::CHUNKS] for k in range(CHUNKS)]):
                facts.update(f)
        extern = regsum.extern_summaries(module)
        s = regsum.solve(facts, extern)
        plans = None
        if with_slots:
            plans, why = slots.solve(facts, s, slots.load_site_targets())
            print(f"{module}: stack slots in {len(plans):,} functions ({sum(sum(isinstance(k, int) for k in p) for p in plans.values()):,} accesses); "
                  f"none in {', '.join(f'{v:,} {k}' for k, v in sorted(why.items(), key=lambda kv: -kv[1]))}")
        regsum.save(module, s, extern, plans)
    return s, plans or {}


def _load_ic(path, starts):
    """Inline-cache sites from tools/icache.py: "site target [target...]" lines (hex). Targets that aren't
    recompiled functions (import thunks) are dropped."""
    ic = {}
    for line in Path(path).read_text().splitlines():
        words = line.split("#")[0].split()
        if not words:
            continue
        site, *ts = (int(w, 16) for w in words)
        ts = [t for t in ts if t in starts]
        if ts:
            ic[site] = ts
    return ic


def _lift_chunk(args):
    k, addrs = args
    out = ['#include "cpu.h"', '#include "decls.h"']
    failed = 0
    stats = [0, 0, 0, 0]
    for a in addrs:
        try:
            lf = FnLifter(_prog, a)
            out.append(lf.lift())
            stats = [x + y for x, y in zip(stats, lf.sync_stats)]
        except Unsupported as e:
            failed += 1
            msg = str(e).replace('"', "'")[:80]
            out.append(f'void F_{a:08x}(CPU *restrict c) {{ guest_unimpl(c, {a:#x}u, "{msg}"); }}')
    (build_dir(_prog.module) / "gen" / f"chunk{k:03}.c").write_text("\n".join(out))
    return failed, stats


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


def _pgo_flags(mode):
    """Compile and link flags for --pgo gen|use (none without --pgo)."""
    if mode == "gen":  # MINA4MAC_PGO_GEN: main.c writes the profile when perfbench stops the game with SIGTERM
        return [f"-fprofile-generate={PGO_RAW}", "-DMINA4MAC_PGO_GEN"]
    if mode == "use":
        if not PGO_DATA.exists():
            sys.exit(f"no {PGO_DATA}: make one with tools/pgo.sh")
        return [f"-fprofile-use={PGO_DATA}"]
    return []


def _cc(job):
    src, extra, opt = job
    # Chunk objects sit next to their sources; runtime objects go in GEN.
    obj = (src.parent if src.name.startswith("chunk") else GEN) / (src.stem + ".o")
    sdl = _sdl_config("--cflags") if src.stem.startswith("sdl2") and src.stem != "sdl2_stdlib" else []
    lua = LUA_CFLAGS if src.name in LUA else []
    subprocess.run(["clang", "-c", f"-{opt}", "-ffp-contract=off", "-fno-strict-aliasing", "-w", *sdl, *lua, *extra,
                    "-I", str(ROOT / "runtime"), "-I", str(GEN), str(src), "-o", str(obj)], check=True)
    return obj


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("modules", nargs="*", choices=MODULES)
    ap.add_argument("--pgo", choices=("gen", "use"), help="build instrumented, or with the merged profile")
    ap.add_argument("--order", action="store_true", help=f"link with {PGO_ORDER.relative_to(ROOT)}")
    ap.add_argument("--opt", choices=("O2", "O3"), default="O2", help="optimization level")
    ap.add_argument("--lto", choices=("thin", "off"), default="thin",
                    help="ThinLTO (default; LuaJIT stays a plain archive): heavy work_ms -3.5%%, build +40 s")
    ap.add_argument("--icprof", action="store_true",
                    help="count indirect calls per (site, target): run with MINA4MAC_ICPROF=<file>, then tools/icache.py")
    ap.add_argument("--ic", default=str(IC_SITES.relative_to(ROOT)),
                    help="inline-cache sites from tools/icache.py ('none' for plain guest_call everywhere)")
    ap.add_argument("--chunking", choices=("roundrobin", "contiguous"), default="roundrobin",
                    help="contiguous keeps neighbours (MSVC links a source file's functions together) in one chunk, so direct calls can inline")
    ap.add_argument("--slots", choices=("on", "off", "check"), default=os.environ.get("MINA4MAC_SLOTS", "off"),
                    help="stack slots in C locals (tools/slots.py; needs --sync live or check): check verifies them at run "
                         "time (exit 13 on a wrong plan)")
    ap.add_argument("--sync", choices=("live", "dirty", "full", "check"), default="live",
                    help="full: store and reload all 8 GPRs at every call; dirty: store only assigned ones; "
                         "live: also skip reloads callee summaries prove unneeded (tools/regsum.py); check: live's code, but "
                         "verify each skipped reload at run time (exit 12 on a wrong summary)")
    args = ap.parse_args()
    modules = args.modules or ["noita", "msvcp120"]
    pgo = _pgo_flags(args.pgo)
    lto = [f"-flto={args.lto}"] if args.lto != "off" else []
    if args.order and not PGO_ORDER.exists():
        sys.exit(f"no {PGO_ORDER}: make one with tools/pgo.sh order")
    GEN.mkdir(parents=True, exist_ok=True)
    starts_by_mod = {}
    for m in modules:
        prog = Program(m)
        prog.is_ordered(prog.t_lo)  # run tools/memorder.py's analysis (or load its cache) before the workers
        starts_by_mod[m] = _module_starts(prog)
    starts = sorted(a for s in starts_by_mod.values() for a in s)
    assert len(starts) == len(set(starts)), "modules overlap"
    ic_path = ROOT / args.ic
    # An --icprof build keeps the inline-cache branches, so its sync counts match the default build's, but every
    # branch calls guest_call_site, so the site profile is the same either way.
    ic = {} if args.ic == "none" or not ic_path.exists() else _load_ic(ic_path, set(starts))
    (GEN / "decls.h").write_text("\n".join(f"void F_{a:08x}(CPU *restrict c);" for a in starts))
    t0 = time.time()
    gen_sdl.generate()  # the bridges' HOST declarations give regsum the imports' stack pops
    gen_gl.generate()
    # msvcp120 first: noita's summaries use its exports'
    sums = {m: regsum_for(m, args.slots != "off") if args.sync in ("live", "check") else ({}, {}) for m in sorted(modules, key=lambda m: m != "msvcp120")}
    tsum = time.time()
    failed = 0
    sync = [0, 0, 0, 0]
    chunk_srcs = []
    for m, ms in starts_by_mod.items():
        mgen = build_dir(m) / "gen"
        mgen.mkdir(parents=True, exist_ok=True)
        for old in mgen.glob("chunk*"):
            old.unlink()
        if args.chunking == "contiguous":
            chunks = [(k, ms[k * len(ms) // CHUNKS:(k + 1) * len(ms) // CHUNKS]) for k in range(CHUNKS)]
        else:
            chunks = [(k, ms[k::CHUNKS]) for k in range(CHUNKS)]
        with mp.Pool(os.cpu_count(), initializer=_init, initargs=(m, args.icprof, ic, args.sync, *sums[m], args.slots)) as pool:
            for f, st in pool.map(_lift_chunk, chunks):
                failed += f
                sync = [x + y for x, y in zip(sync, st)]
        chunk_srcs += sorted(mgen.glob("chunk*.c"))
    t1 = time.time()
    table = ['#include "rt.h"', '#include "decls.h"',
             "const FnEntry FN_TABLE[] = {" + ",".join(f"{{{a:#x}u,F_{a:08x}}}" for a in starts) + "};",
             f"const int FN_COUNT = {len(starts)};"]
    ic_fns = sorted({t for ts in ic.values() for t in ts})  # rt_hook warns about these: cached sites bypass hooks
    table += ["const uint32_t IC_TARGETS[] = {" + ",".join([f"{t:#x}u" for t in ic_fns] + ["0"]) + "};",
              f"const int IC_TARGET_COUNT = {len(ic_fns)};"]
    (GEN / "table.c").write_text("\n".join(table))
    build_luajit()
    launcher = [*LAUNCHER, *SDL, *LUA, "sdl2_gen.c", "gl_gen.c"]
    srcs = chunk_srcs + [GEN / "table.c", GEN / "sdl2_gen.c", GEN / "gl_gen.c"] + [ROOT / "runtime" / n for n in ("rt.c", "harness.c", "bench.c", *LAUNCHER, *SDL, *LUA)]
    with mp.Pool(os.cpu_count()) as pool:
        objs = pool.map(_cc, [(s, pgo + lto, args.opt) for s in srcs])
    t2 = time.time()
    only = {"harness", "bench"} | {Path(n).stem for n in launcher}
    common = [str(o) for o in objs if o.stem not in only]
    exe = ROOT / "build/harness_all"
    link = ["clang", *pgo[:1]]  # the instrumented build links the profile runtime
    if lto:  # the link does the codegen; the cache lets the 3 links share it. ld64 ignores -O here (and
        # -Wl,-mllvm,-O3): the compile-time --opt still changes the bitcode, fp-contract/aliasing ride along in it.
        LTO_CACHE.mkdir(parents=True, exist_ok=True)
        link += [*lto, f"-Wl,-cache_path_lto,{LTO_CACHE}"]
    if args.order:  # the hot functions of the launcher; harness and bench just get the same layout
        link += [f"-Wl,-order_file,{PGO_ORDER}"]
    subprocess.run([*link, *common, str(GEN / "harness.o"), "-o", str(exe)], check=True)
    subprocess.run([*link, *common, str(GEN / "bench.o"), "-o", str(ROOT / "build/bench")], check=True)
    subprocess.run([*link, *common, *(str(GEN / (Path(n).stem + ".o")) for n in launcher),
                    str(LUAJIT_LIB), *_sdl_config("--libs"), "-framework", "OpenGL", "-framework", "CoreVideo", "-o", str(ROOT / "build/mina4mac")], check=True)
    t3 = time.time()
    src_mb = sum(s.stat().st_size for s in chunk_srcs) / 1e6
    print(f"functions {len(starts):,} (stubbed {failed:,}); MINA4MAC_TSO={prog.tso_mode}; pgo {args.pgo or 'off'}{', order file' if args.order else ''}; -{args.opt}{', lto ' + args.lto if lto else ''}; {args.chunking} chunks; "
          f"{'icprof' if args.icprof else f'inline caches {len(ic)} sites'}; sync {args.sync} (MINA4MAC_VCALL={regsum.VCALL_MODE}): "
          f"{sync[1] / max(sync[0], 1):.2f} stores/call site, {sync[3] / max(sync[2], 1):.2f} reloads/return (of 8); "
          f"slots {args.slots} ({sum(sum(isinstance(k, int) for k in p) for _, plans in sums.values() for p in plans.values()):,} accesses)")
    print(f"regsum {tsum - t0:.0f}s, lift {t1 - tsum:.0f}s, compile {t2 - t1:.0f}s, link {t3 - t2:.0f}s; C source {src_mb:.0f} MB; "
          f"binary {exe.stat().st_size / 1e6:.1f} MB")


if __name__ == "__main__":
    main()
