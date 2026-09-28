# mina4mac, part 3: performance

This continues `PLAN2.md`, which is complete: the game is playable from the packaged `build/Noita.app`, and a
2026-09-27 user playtest (audio, rebinding, focus pause, Continue) found nothing wrong. **Read the Context sections of
`PLAN.md` and `PLAN2.md` first.** Their rules (bring-your-own exe, never commit game or FMOD files,
`tools/check.sh` before every commit, record results in the task notes) all still apply.

Performance is the reason this project exists: Wine + Rosetta dropped frames under simulation load. The game is
already faster than the Sikarugir Wine build. This plan aims to make heavy scenes hold 60 fps with headroom.

## Context

### Where PLAN2 stopped (perf task notes, 2026-09-26)

- `tools/perfbench.sh` runs a seed-pinned water/oil/lava flood for 1800 frames (`-gamemode 0` skips the menu;
  `UNCAPPED=1` turns vsync off and caps at 1000 fps). Native: 55.7 fps capped, 57–65 uncapped. Wine: 50.9 / 42.8.
- Runs vary by about ±7%, so A/B comparisons need interleaved runs and medians. `build/ab1_*.txt` holds the last A/B.
- `sample` profile: `guest_call` takes ~15% of guest CPU. The main thread spends ~21% in a `Sleep(0)`-style wait
  for 8 job workers, which are only ~39% busy.
- Inlining `guest_call` at call sites was ~5% **slower** (+4 MB of code) and was reverted.
- TSO ordering (`MINA4MAC_TSO=auto`) costs nothing measurable: no hot function is ordered.
- `MINA4MAC_FPS=1` logs fps and average/worst frame time once a second.
- The user has never seen the SwapWindow stalls (minutes inside `_CGSWindowIsOrderedIn`) in normal play. They seem
  specific to unattended launches, like an occluded or never-focused window. Treat a stalled benchmark run as
  invalid, not as a finding.

### Facts gathered while writing this plan

- **Dispatch.** `guest_call` (`runtime/rt.c:266`) calls `rt_lookup`, a two-level table (a page pointer, then a slot),
  and falls back to `call_thunk` for imports. The lookup is already cheap, so its 15% is probably the indirect
  branch itself: mispredictions and the loss of return prediction across `F_*` calls. Direct calls
  (`lift.py call_direct`) already call `F_<addr>(c)` directly.
- **Register sync.** Every call site, direct or indirect, stores all 8 GPRs to `CPU` before the call and reloads
  all 8 after it (`sync_out`/`sync_in`), and pushes the return address to guest memory. That puts 16 memory
  operations plus a store around every call, and clang can't keep values in registers across calls.
- **x87.** The stack lives in `c->st[8]` (doubles in the `CPU` struct), not in C locals, so every x87 instruction
  is a load or store through `c`. `fpu_round` switches on the control word for every `fist`.
- **Compiler flags.** (Superseded by Phase 10: ThinLTO is now the default, and guest memory sits at a fixed address.)
  Everything is built with `clang -O2 -ffp-contract=off -fno-strict-aliasing`, with no LTO, no
  PGO and no order file. The binary is 50 MB, so i-cache and iTLB pressure is plausible.
- **CPU topology.** `GetSystemInfo` reports `sysconf(_SC_NPROCESSORS_ONLN)`, which is 10 on this M1 Max (8 P-cores
  + 2 E-cores). Guest threads are plain `pthread_create` threads (`runtime/thread.c:79`) with no QoS class set.
  If the job system sizes its pool from the CPU count and a job lands on an E-core, that job takes several times
  longer and the main thread waits for it. This fits "main waits 21%, workers 39% busy".
- **Yield paths.** `__crtSleep(ms)` → `usleep(ms*1000)` (so `Sleep(0)` is `usleep(0)`), and the ConcRT
  `_Context::_Yield` → `sched_yield()`. The job wait loop is `while (job->pending > 0) _Thrd_yield();` at 0x726a51.

### Key design decisions

- **Measure before changing anything.** Every task below starts with a hypothesis and a number, and ends with an
  interleaved A/B (at least 3 runs per side, medians). A change that doesn't move the median beyond the noise
  gets reverted, unless it's needed by a later task. Record the numbers in the task notes, including failures.
- **Correctness gates stay.** Lifter changes must pass `tools/check.sh`, difftest and the determinism check
  (`tools/determinism.sh`) before they're measured. Speed that changes world generation doesn't count.
- **Cheapest levers first:** scheduling and compiler flags (no lifter changes), then call-boundary codegen, then
  deeper lifter work.

## Tasks

### Phase 8: a trustworthy benchmark

- [x] Make perfbench measure CPU work, not just fps.
  - `MINA4MAC_FRAMELOG` (sdl2.c) + mod marks → work_ms/cpu_ms/swap_ms in perfbench output; `SCENE=heavy`; `tools/perfab.sh A B [n]`. The old ±7% noise was mostly runs continuing the previous run's world; `run` now deletes save00's world/player/world_state.
  - Same binary, 6 runs: flood work 14.82–15.00 ms (51 fps, swap ~5 ms, so frames miss vsync), heavy 22.94–23.56 ms (35 fps). Results in build/perfab/.
  - At 60 Hz capped, fps saturates, and uncapped still includes GL and swap time. Add a per-frame CPU-time metric
    (main-thread time from frame start to before `SDL_GL_SwapWindow`, via the hand-written SwapWindow in
    `runtime/sdl2.c`) and report its median and p95 alongside fps.
  - Add a second, heavier scene (for example many physics bodies + explosions, or the flood at 2x) so a single
    change shows up above the ±7% noise.
  - Add `tools/perfab.sh A B [n]`: build or take two binaries and run n interleaved pairs, then print the medians
    and the spread. A/B must be one command.
  - Check that the numbers are stable: 5 runs of the same binary should land within a few percent on CPU time.
- [x] A repeatable profile.
  - `tools/perfprof.sh` (sample every 5 ms during perfbench; 1 ms slows the game ~25%) → `tools/perfprof.py` buckets/threads/waits/top fns; `tools/fninfo.py <addr>` names functions (RTTI vtables, callers). Guest threads are now named `guest N <start>`. Baseline + top 20 in PROFILE.md.
  - Findings: only 9 of 18 guest threads work (34–49% busy); main thread spends 41–50% in Sleep(0) from `_Thrd_yield`; dispatch is 12–13% of busy time, mostly vcalls to tiny cell getters in the cell sim (≈60% of work in its top 20).
  - Script `sample` (or `xctrace` with Time Profiler) over the benchmark's steady-state window. Aggregate by
    `F_<addr>` and by runtime symbol, per thread (main, the 8 workers, the FMOD thread).
  - Group the costs into buckets: lifted guest code, `guest_call`/dispatch, x87 helpers, HLE (msvcr/kernel32/sync),
    Lua bridge + LuaJIT, GL bridge + driver, waiting (yield, usleep, condvars). This table is the baseline for
    every later task.
  - Name the top 20 guest functions by address with a short guess at what they are (cell simulation, Box2D, render
    batching…), from strings, callers and the Lua API they serve.

### Phase 9: scheduling (no codegen changes)

- [x] Why does the main thread wait?
  - `MINA4MAC_JOBLOG=<file>` (runtime/joblog.c, `rt_hook` wraps every std::function target) + `tools/joblog.py log --framelog frames`; results in PROFILE.md "Job system", logs in build/joblog/. Pool = 9 workers (10 CPUs − 1) + an idle second pool of 9.
  - Main waits on the 4 chunk-update passes (0x726a5e, 8–13 ms/frame), bound by the pass's longest chunk job / total work, not wake latency (~5 µs). Main's `Sleep(0)` is a spin, holding a core, so ~13% of job time lands on E-cores (0–1), where jobs are several times slower.
  - Log the job system: how many workers the game creates, the job sizes, and how long each worker is idle vs
    busy per frame. Hook at the job wait loop (0x726a51) and the worker loop.
  - Check which cores the workers run on (`powermetrics` or per-thread CPU time vs wall time).
- [x] Try the scheduling fixes. A/B each on its own:
  - `runtime/sched.c`: `MINA4MAC_CPUS=all|pcores|<n>`, `MINA4MAC_QOS=default|interactive|initiated`, `MINA4MAC_YIELD=nap<us>|usleep|sched|spin`; results table in PROFILE.md "Scheduling", logs in build/sched/.
  - Kept: the job wait's `Sleep(0)` naps 20 µs (new default) instead of spinning: heavy work_ms −4.9% (p95 −6.9%), fps +3.7%, main CPU −57%; flood unchanged (−1.2%, swap-bound). E-core job time 13% → 10.4%.
  - Reverted (knobs stay, off): QoS (user-initiated adds −0.4% on top of nap20), P-cores only (−2%, noise), 7 CPUs (+3.9% worse), `sched_yield` and spin-then-yield (−3.3%, −1.8%). sync.c wake latency is 5 µs median: nothing to fix.
  - Set `QOS_CLASS_USER_INTERACTIVE` (or `USER_INITIATED`) on the main and guest worker threads, so they stay on
    P-cores.
  - Report only the P-core count (`hw.perflevel0.physicalcpu`) from `GetSystemInfo`, so the pool doesn't
    oversubscribe the E-cores. Keep an env override.
  - Make the yields cheaper and fairer: `usleep(0)` is a syscall that may give up the rest of the quantum. Try a
    short spin with `__builtin_arm_yield()`/`wfe` before `sched_yield`, only in `__crtSleep(0)` and `_Thrd_yield`.
  - Check the Win32 event/condvar/critical-section HLE (`runtime/sync.c`) for wake latency: a worker woken late
    looks exactly like "workers idle, main waiting".

### Phase 10: compiler-level wins (no lifter changes)

- [x] Profile-guided optimization. Build with `-fprofile-instr-generate`, run perfbench plus a menu → new game →
  play sequence, merge with `llvm-profdata`, and rebuild with `-fprofile-instr-use`. This should fix block layout
  and inlining in hot `F_*` functions and give the most gain per unit of effort. Document the workflow in the README,
  because profiles are derived from the user's own game and must not be committed.
  - `tools/pgo.sh [gen|train|merge|order|use]`, `tools/build_all.py --pgo gen|use`. IR PGO (`-fprofile-generate`), trained on perfbench flood + heavy only (the menu needs clicks).
    The instrumented game is ~12× slower (flood 190 ms/frame, counter contention across 9 workers), so heavy stops at perfbench's
    5-minute cap (~990 frames); main.c writes the profile on SIGTERM (`MINA4MAC_PGO_GEN`). Training ≈ 10 min, build 2.5 min.
    No profile mismatch warnings; binary 51.4 → 45.7 MB.
  - Heavy A/B (build/perfab/20260927-152744, 3 pairs): fps +1.4%, work_ms −1.7% (ranges overlap), cpu_ms −1.4%. Noise-level.
  - Why so little: the profile's top 10 functions are 53% of executed blocks, led by `guest_call` (3.8e9 calls), which is already a
    tight tail call. The cost is the call boundary (Phase 11), which PGO can't see through.
  - Kept as opt-in tooling (README); the default build doesn't use it. **`tools/check.sh` not yet run on these changes** (it opens
    the game window; the default build only changes by an `#ifdef`'d SIGTERM handler, compile-checked both ways).
  - Hot imports from the profile, for Phase 12: `call_thunk` 480M calls; `fgetc` 100M (loading), `QueryPerformanceCounter` 67M and
    `__crtSleep` 18M (wait loops, inflated by the slow run), `floor` 38M, `operator new/delete` 30M/27M, libm sqrt/sin/cos 17–21M,
    memset/memcpy 16–19M.
- [x] Code layout: an order file (`-Wl,-order_file`) from the profile, to cluster hot functions and cut i-cache and
  iTLB misses in the 50 MB binary. Try it with and without PGO.
  - `tools/pgo_order.py` (all 14,469 executed functions, by block-count sum; the top 1,000 are 99.7% of it), `build_all.py --order`.
  - PGO + order vs no PGO, heavy, 4 pairs (build/perfab/20260927-154039): fps +0.9%, work_ms −0.3%, cpu_ms −7.2%, cpu_p95 −5.2%.
    Main-thread CPU drops, but main mostly waits for the workers, so frame time doesn't move.
  - Order only vs neither, heavy, 4 pairs (build/perfab/20260928-112254): fps +1.0%, work_ms −0.7% (ranges overlap), work_p95 −1.9%,
    cpu_p95 −5.5%. Noise-level, like PGO: layout isn't the bottleneck. Both stay opt-in (`--order`); the default build uses neither.
  - `tools/check.sh` passed on the Phase 10 commit (2026-09-28).
- [x] Try `-O3`, and ThinLTO over the chunks (build time matters: record it).
  - Likely the more useful of the two: `build_all.py` spreads functions round-robin over 128 chunks, so a direct callee is almost
    never in its caller's chunk and can't be inlined (e.g. `GetCellPtr`, 3%). Grouping callers and callees into the same chunk is a cheaper
    alternative to try.
  - **Kept: ThinLTO is now the default** (`build_all.py --lto thin|off`, cache in build/lto_cache/). Build 2:06 → 2:48 (compile 107 →
    65 s, link 1 → 82 s for the three links sharing the cache), binary 51.4 → 59.6 MB. Heavy, 4 pairs vs `-O2`
    (build/perfab/20260928-113504): fps +2.4%, work_ms −3.5%, work_p95 −4.9% (ranges don't overlap), cpu_ms −3.6%. The LTO side's
    median repeated in the next batch (22.69 vs 22.70 ms).
  - `-O3` (`--opt O3`) on top of ThinLTO: work_ms +1.2%, fps −0.8% (build/perfab/20260928-120549): noise, dropped. ld64 ignores `-O`
    at link time and `-Wl,-mllvm,-O3` too (byte-identical output), so `--opt` only changes the pre-link pipeline.
  - Contiguous chunks without LTO (`--chunking contiguous`: adjacent functions, since MSVC links a source file's functions
    together): work_ms −4.0% but ranges overlap and work_p95 −0.7% (build/perfab/20260928-114903); the chunks are unbalanced (largest
    32 MB of C vs 3.3 MB mean), so compile takes 256 s. Not kept; ThinLTO does the same job and is steadier. The option stays.
  - Determinism: `tools/determinism.sh run [binary]` is now unattended (`-gamemode 0` on a fresh run, perfbench mod paused).
    Two runs of the same `-O2` binary differ in ~50 lines at f60 and f600 (physics bodies, vines, a few liquid cells): only
    seed/RNG/libm are exact, so `diff` now reports those exactly and the snapshot sections as counts. ThinLTO vs `-O2`: seed, RNG
    and libm identical; f60 37–61 and f600 34–46 differing lines, the same band as `-O2` vs `-O2` (build/det/). PLAN2's
    "identical at +60/+600" doesn't reproduce even for `-O2` now; the saved 2026-09-26 mina4mac file differs from Wine's in 335 lines
    there, so the files kept from that check don't support it either.
- [x] Check whether `-fno-strict-aliasing` and `-ffp-contract=off` can be narrowed. Keep `fp-contract=off` for x87
  and SSE code (determinism), but check the ones that only guard integer code.
  - Neither flag matters, so both stay. `-fstrict-aliasing` gives byte-identical chunk code, because every guest access
    goes through `char` or the `may_alias` typedefs in cpu.h. `-ffp-contract` only touches FP expressions: over 4 chunks, `off` and
    `on` both give 0 FMAs (each lifted instruction is its own statement), and only `fast` fuses (5–22 per chunk, all in FP code,
    so results would change).
  - What the disassembly showed instead: `MEM` was a global pointer, and a `char`/`may_alias` store may alias it, so clang
    reloaded `MEM` after **every** guest store. **Kept:** rt_init maps guest memory at a fixed host address (`MEM_HOST_BASE`
    0x200000004000, an mmap hint, checked), and `MEM` is a constant. The base is a 16 KB page off a round number: with the low 32
    bits zero, clang builds each address with an ORR immediate plus a separate access, not `[base, w, uxtw]`. chunk000 `__TEXT`
    268.6 → 252.9 KB (round base) → 244.0 KB (−9.2%); binary 59.6 → 55.3 MB.
  - Heavy, 4 pairs (build/perfab/20260928-135633): work_ms −2.0% (B faster in all 4 pairs), work_p95 −2.3% (ranges don't
    overlap), fps +1.3%, cpu_ms +0.8% (noise). `tools/check.sh` passed. Determinism (build/det/memfix_{1,2}.txt): seed/RNG/libm
    identical to the ThinLTO baseline; snapshot diffs f60 69–70 and f600 54 lines vs a same-binary band of 49–59 and 43–45.
    That's slightly above the band, but those sections vary run to run (f1 21–46 even for one binary).

### Phase 11: the call boundary

Plan (2026-09-27): after ThinLTO, do the per-site inline caches first (about half a session) as the checkpoint. If they give ≥5% on
heavy, do register-sync liveness (1–2 sessions, riskiest); if not, re-profile before investing there. Estimate for the whole phase: 2–4
sessions. Making `tools/determinism.sh` run unattended (like perfbench with `-gamemode 0`) would make lifter changes cheaper to verify.

- [ ] Cheaper register sync.
  - Use liveness at call sites: don't reload registers the caller overwrites before reading them, and don't store
    registers the callee never reads. Every compiler-generated callee in the exe follows the MSVC convention
    (eax/ecx/edx volatile; ebx/esi/edi/ebp preserved). Verify that per function by analysis, not by assumption,
    and fall back to the full sync for anything unproven (hand-written asm, `/Oy-` oddities, SEH helpers).
  - Measure the static and dynamic count of removed loads and stores first.
- [ ] Indirect-call dispatch.
  - Resolve known targets at lift time: vtable calls whose table is a constant in `.rdata`, and calls through IAT
    slots to recompiled modules (msvcp120 → direct `F_` calls instead of `guest_call`).
  - For the remaining hot sites, try a per-site inline cache (`if (t == last) F_last(c); else guest_call(...)`),
    measured against the failed full-inline attempt. Only the profile's top sites, not all of them.
- [ ] Return-address handling: check whether pushing the return address to guest memory is needed for every call.
  Keep it wherever the callee may read it (`[esp]` access, SEH, `_alloca` probes, `__security_check_cookie`).

### Phase 12: hot-spot specific

- [ ] x87 in locals. Lift x87 stack slots as C `double` locals within a function (TOP known statically in most
  blocks), and sync to `c->st` only at calls and at blocks with unknown TOP. Cache the rounding mode per function
  when no `fldcw` is in it. Only if the profile shows x87-heavy functions near the top.
- [ ] SSE/memcpy paths: check that hot `memcpy`/`memset` (msvcr120 HLE) and `rep movs` lifting use the host
  libc/NEON paths.
- [ ] Bridge overhead: Lua (`runtime/lua51.c`) and GL (`runtime/opengl32.c`) calls per frame, and the cost per
  call. Batch or shortcut the hot ones (for example, stop re-checking the current context on every GL call).
- [ ] Whatever the Phase 8 profile puts on top that the tasks above don't cover.

### Phase 13: wrap up

- [ ] Final numbers: native vs Wine, capped and uncapped, both scenes, plus CPU time per frame. Compare to the PLAN2
  baseline and record which changes contributed and by how much.
- [ ] Rebuild `build/Noita.app` (PGO profile included in the local build if Phase 10 kept it) and do a user
  playtest in a heavy scene.
