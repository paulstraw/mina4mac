"""Summarize a macOS `sample` report of build/mina4mac (tools/perfprof.sh takes one during perfbench).

  uv run tools/perfprof.py build/perfprof/<run>/sample.txt [--top 30]

Every sample of every thread is a leaf of sample's call graph; each goes into one bucket:
  guest noita / guest msvcp120   lifted code (F_<addr>; x87 and SSE are inlined there, so they don't show apart)
  dispatch                       runtime/rt.c: guest_call, call_thunk, the lookup table
  math HLE                       runtime/msvcr120_math.c (_CIpow, _ftol...)
  HLE                            the other msvcr120/kernel32/sync/heap/... hostcalls
  Lua                            runtime/lua51.c, LuaJIT (and JIT-compiled code under it)
  GL                             runtime/opengl32.c, gl_gen.c and Apple's GL stack below them
  GL swap                        everything under SDL_GL_SwapWindow (driver work and vsync waits)
  SDL / FMOD                     sdl2*.c and libSDL2; fmod*.c and libfmod*
  waiting                        blocked in the kernel (condvar, usleep, sched_yield, mutex, mach_msg...)
  other                          anything else (AppKit, CoreAudio, dispatch queues...)
A system-library leaf that isn't a wait (memmove, malloc, ...) counts for the bucket of the nearest mina4mac frame
above it. Symbols are mapped to runtime files with nm over build/gen_all/*.o and LuaJIT's libluajit.a.
"""
import argparse
import collections
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GL_LIBS = {"AppleMetalOpenGLRenderer", "GLEngine", "Metal", "IOGPU", "libGL.dylib", "OpenGL", "IOAccelerator",
           "GLRendererFloat", "libGFXShared.dylib", "IOSurface", "libCVMSPluginSupport.dylib", "libGLImage.dylib"}
WAITS = {"__psynch_cvwait", "__psynch_mutexwait", "__psynch_rw_rdlock", "__psynch_rw_wrlock", "__semwait_signal",
         "swtch_pri", "thread_switch", "__ulock_wait", "__ulock_wait2", "semaphore_wait_trap",
         "semaphore_timedwait_trap", "semaphore_wait_signal_trap", "mach_wait_until", "start_wqthread", "mach_msg2_trap", "mach_msg_trap", "__workq_kernreturn", "kevent", "kevent_id",
         "kevent_qos", "__select", "__sigwait", "__wait4", "read", "__read_nocancel"}
FILE_BUCKET = {"rt": "dispatch", "msvcr120_math": "math HLE", "lua51": "Lua", "opengl32": "GL", "gl_gen": "GL",
               "sdl2": "SDL", "sdl2_gen": "SDL", "fmod": "FMOD", "fmod_stub": "FMOD"}
BUCKETS = ["guest noita", "guest msvcp120", "dispatch", "math HLE", "HLE", "Lua", "GL", "GL swap", "SDL", "FMOD",
           "waiting", "other"]
SWAP = "hostcall_SDL2_SDL_GL_SwapWindow"


def symbol_files():
    """mina4mac symbol (without the leading _) -> runtime file stem, or 'luajit'."""
    out = {}
    objs = sorted((ROOT / "build/gen_all").glob("*.o"))
    lib = ROOT / "build/luajit/src/libluajit.a"
    for path, stem in [(o, o.stem) for o in objs] + ([(lib, "luajit")] if lib.exists() else []):
        nm = subprocess.run(["nm", "-P", str(path)], capture_output=True, text=True).stdout
        for line in nm.splitlines():
            f = line.split()
            if len(f) >= 2 and f[1] in "tT" and f[0].startswith("_"):
                out.setdefault(f[0][1:].split(".cold")[0], stem)
    return out


LINE = re.compile(r"^( {4}[ +!:|]*?)(\d+) (.*)$")
FRAME = re.compile(r"^(.*?)\s+\(in ([^)]+)\)")


def parse(path):
    """-> [(thread name, [(count, [(sym, lib), ...root..leaf]), ...])]: the self samples of each call-graph leaf."""
    threads, cur, stack, on = [], None, [], False

    def pop_to(depth):
        while stack and stack[-1][0] >= depth:
            d, n, kids, frame = stack.pop()
            if n > kids and cur is not None and d > 0:
                cur[1].append((n - kids, [s[3] for s in stack[1:]] + [frame]))
            if stack:
                stack[-1][2] += n
    for line in open(path, errors="replace"):
        if line.startswith("Call graph:"):
            on = True
            continue
        if not on:
            continue
        if not line.startswith("    ") or line.startswith("Total number"):
            if line.strip() == "" or line.startswith("Total number"):
                pop_to(0)
                if line.startswith("Total number"):
                    break
            continue
        m = LINE.match(line.rstrip("\n"))
        if not m:
            continue
        depth, n, text = (len(m.group(1)) - 4) // 2, int(m.group(2)), m.group(3)
        pop_to(depth)
        if depth == 0:
            name = re.sub(r"^Thread_[^\s:]+:?\s*", "", text).strip() or text.split()[0]
            cur = (name, [])
            threads.append(cur)
            stack.append([0, n, 0, None])
            continue
        fm = FRAME.match(text)
        frame = (fm.group(1).strip(), fm.group(2)) if fm else (text.split("  [")[0].strip(), "?")
        stack.append([depth, n, 0, frame])
    pop_to(0)
    return threads


def sym_bucket(sym, files):
    if sym.startswith("F_"):
        return "guest msvcp120" if int(sym[2:10], 16) >= 0x10000000 else "guest noita"
    if sym.startswith("OUTLINED_FUNCTION"):
        return "guest noita"
    f = files.get(sym)
    if f is None:
        return None
    return "Lua" if f == "luajit" else FILE_BUCKET.get(f, "HLE")


def classify(path, files):
    """-> (bucket, wait site): the site is the nearest runtime (non-F_) frame and guest frame above a wait."""
    syms = [s for s, _ in path]
    if SWAP in syms:
        return "GL swap", None
    sym, lib = path[-1]
    ours = [(s, sym_bucket(s, files)) for s, l in reversed(path) if l == "mina4mac" and not s.startswith("DYLD-STUB")]
    if lib == "mina4mac" and not sym.startswith("DYLD-STUB"):
        return sym_bucket(sym, files) or "HLE", None
    if sym in WAITS:
        host = next((s for s, b in ours if not s.startswith("F_")), "-")
        guest = next((s for s, b in ours if s.startswith("F_")), "-")
        return "waiting", (host, guest)
    below = []  # frames between the leaf and the nearest mina4mac frame
    for s, l in reversed(path):
        if l == "mina4mac":
            break
        below.append(l)
    if any(l in GL_LIBS or l.startswith("AGX") for l in below):
        return "GL", None
    if any(l.startswith("libfmod") for l in below):
        return "FMOD", None
    if any(l.startswith("libSDL2") for l in below):
        return "SDL", None
    for s, b in ours:
        if b:
            return b, None
    return "other", None


def group(name, leaves):
    if name.startswith("Main Thread"):
        return "main"
    if name.startswith("guest "):
        return "guest"
    if any(l.startswith("libfmod") for _, p in leaves for _, l in p[:6]):
        return "FMOD"
    return "other"


def pct(n, d):
    return f"{100 * n / d:5.1f}" if d else "    -"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("sample")
    ap.add_argument("--top", type=int, default=30)
    a = ap.parse_args()
    files = symbol_files()
    threads = parse(a.sample)
    ticks = max(sum(n for n, _ in leaves) for _, leaves in threads)
    print(f"{a.sample}: {len(threads)} threads, {ticks} samples per thread")

    bygroup = collections.defaultdict(collections.Counter)  # group -> bucket -> samples
    nthreads = collections.Counter()
    per_thread = []
    waits = collections.Counter()
    self_fn = collections.defaultdict(collections.Counter)  # sym -> group -> samples
    incl = collections.defaultdict(collections.Counter)  # guest sym -> group -> busy samples with it on the stack
    for name, leaves in threads:
        g = group(name, leaves)
        nthreads[g] += 1
        tb = collections.Counter()
        for n, path in leaves:
            b, site = classify(path, files)
            tb[b] += n
            if site:
                waits[(g, *site)] += n
            if b == "waiting":
                continue
            if path[-1][1] == "mina4mac":
                self_fn[path[-1][0]][g] += n
            for s in {s for s, _ in path if s.startswith("F_")}:
                incl[s][g] += n
        bygroup[g].update(tb)
        per_thread.append((name, g, sum(tb.values()), tb))

    groups = [g for g in ("main", "guest", "FMOD", "other") if nthreads[g]]
    busy = sum(n for g in groups for b, n in bygroup[g].items() if b != "waiting")
    print(f"busy (not waiting) {busy / ticks:.2f} cores on average")
    print("\n## Buckets (cores: samples / samples per thread; share = % of all busy samples)\n")
    print(f"{'bucket':16}" + "".join(f"{g + f' ({nthreads[g]})':>12}" for g in groups) + f"{'share':>8}")
    for b in BUCKETS:
        tot = sum(bygroup[g][b] for g in groups)
        if tot:
            print(f"{b:16}" + "".join(f"{bygroup[g][b] / ticks:12.2f}" for g in groups)
                  + f"{pct(tot, busy) if b != 'waiting' else '':>8}")

    print("\n## Threads (% of the thread's samples; busy = not waiting)\n")
    for name, g, total, tb in per_thread:
        if total:
            top = ", ".join(f"{b} {pct(n, total).strip()}" for b, n in tb.most_common(4) if n)
            print(f"{name[:34]:34} {g:6} busy {pct(total - tb['waiting'], total)}  {top}")

    print("\n## Waiting by site (group, nearest runtime frame, nearest guest frame; cores)\n")
    for (g, host, guest), n in waits.most_common(12):
        print(f"{g:6} {n / ticks:6.2f}  {host:40} {guest}")

    def fn_table(title, items):
        print(f"\n## {title} (% of all busy samples)\n")
        print(f"{'function':42}{'all':>7}" + "".join(f"{g:>7}" for g in groups))
        for s, c in items:
            print(f"{s[:42]:42}{pct(sum(c.values()), busy):>7}" + "".join(f"{pct(c[g], busy):>7}" for g in groups))

    def top(d, keep, k):
        return sorted(((s, c) for s, c in d.items() if keep(s)), key=lambda x: -sum(x[1].values()))[:k]
    fn_table(f"Top {a.top} guest functions by self time", top(self_fn, lambda s: s.startswith("F_"), a.top))
    fn_table("Top 15 runtime/LuaJIT symbols by self time", top(self_fn, lambda s: not s.startswith("F_"), 15))
    fn_table(f"Top {a.top} guest functions by inclusive busy time", top(incl, lambda s: True, a.top))

if __name__ == "__main__":
    main()
