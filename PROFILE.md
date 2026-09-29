# Profile baseline (PLAN3 Phase 8)

Taken 2026-09-27 on an M1 Max with `tools/perfprof.sh` (`sample` every 5 ms, starting 5 s after the perfbench
start mark), commit 1a2a503 plus the Phase 8 benchmark changes. Raw reports: `build/perfprof/20260927-122740/`
(flood, 20 s) and `build/perfprof/20260927-122839/` (heavy, 30 s). Re-summarize a sample with
`uv run tools/perfprof.py <sample.txt>`, and look up a function with `uv run tools/fninfo.py <addr> [-d]`.
At 5 ms, sampling doesn't measurably slow the game (the fps per 300 frames match unsampled runs). At 1 ms it
costs about 25%.

## Buckets

Cores = samples in the bucket / samples per thread, so 1.00 is one thread busy the whole time. Share = % of all
non-waiting samples.

| bucket | flood main | flood guest threads | flood share | heavy main | heavy guest threads | heavy share |
|---|---|---|---|---|---|---|
| lifted noita code (x87/SSE inlined here) | 0.18 | 2.58 | 74.1% | 0.20 | 3.69 | 78.6% |
| lifted msvcp120 | 0.01 | 0.01 | 0.4% | 0.01 | 0.01 | 0.3% |
| dispatch (`guest_call`, `call_thunk`) | 0.02 | 0.44 | 12.6% | 0.03 | 0.63 | 13.3% |
| math HLE (msvcr120_math) | 0.00 | 0.00 | 0.2% | 0.00 | 0.01 | 0.3% |
| other HLE (msvcr120/kernel32/sync/heap) | 0.06 | 0.05 | 3.1% | 0.06 | 0.05 | 2.2% |
| Lua bridge + LuaJIT | 0.00 | 0.00 | 0.0% | 0.00 | 0.00 | 0.0% |
| GL bridge + driver | 0.04 | 0.00 | 1.3% | 0.03 | 0.00 | 0.7% |
| GL swap (`SDL_GL_SwapWindow`) | 0.23 | 0.00 | 6.3% | 0.14 | 0.00 | 2.8% |
| FMOD (its own 7 threads) | – | – | 2.0% | – | – | 1.7% |
| waiting | 0.45 | 14.91 | | 0.53 | 13.61 | |
| total busy (all threads) | | | 3.73 cores | | | 4.95 cores |

- x87 and SSE are inlined into the lifted functions, so sampling can't separate them from other guest code. A
  per-instruction view (Instruments, or counting x87 ops in the top functions) would be needed.
- Lua is idle in these scenes. GL outside the swap is small.

## Threads

- **18 guest threads**, all started through msvcp120's `std::thread` launch pad (`F_1802f325`). **Only 9 do work**:
  in flood each is 34–41% busy, in heavy 48–49%. The other 9 stay under 1% busy (a second pool, maybe the ConcRT
  scheduler). The 9 workers wait in `_Cnd_wait` (`sync_wait` from `F_1802ea37`) for the rest of the time.
- **Main thread**: flood 55% busy (swap 23%, guest 18%, HLE 6%), heavy 47% busy (guest 20%, swap 14%). It
  spends **41–50% in `Sleep(0)`** (`__crtSleep` → `usleep(0)`, called from `_Thrd_yield` at `F_1802eda9`) while
  it waits for jobs. That is the job wait loop from PLAN3's facts.
- Waits on `cs_lock` (critical sections) are small: 0.03 cores on main, 0.06–0.07 across the workers.
- So at most 5 of 10 cores are busy. The workers are idle half the time and the main thread mostly waits. Both
  point at scheduling (Phase 9) as much as at code speed.

## Where the work is

Almost all worker time is the falling-sand cell simulation, run as jobs: `F_0096b480` (a launch-pad lambda) →
`F_00849bc0` (worker loop: mutex, condvar, `std::function`) → the job functor `F_009b0bd0` (`Job@jobs`) →
`F_007288a0` (a chunk update in `GridWorldThreaded`), which is 66% (flood) and 73% (heavy) of all busy samples. Tile rendering
(`GridWorld::RenderTiles` → `F_00722cd0`) is a second job type (9–10%). The main thread's work sits under
`F_0080b6f0` (game loop) → `F_00dd98a0` (frame).

**The hot code is dominated by tiny virtual calls.** The cell updaters call per-cell virtual methods of 2–40
instructions (`ICell` getters such as `F_004ac0f0` = `mov eax,[ecx+0x14]; ret`, material lookups through vtable
slot 0x30, bounds checks) through `guest_call`. `guest_call`'s own self time (12.1% flood, 12.9% heavy) is about the same as the top
guest function. This is the case for Phase 11 (register sync and indirect-call dispatch): a devirtualized or
inline-cached vcall to a 2-instruction getter would remove most of that cost.

### Top 20 guest functions (self time, % of all busy samples)

| # | function | flood | heavy | what it is (from RTTI vtables, callers, disassembly) |
|---|---|---|---|---|
| 1 | `F_0070a520` | 10.9 | 10.4 | liquid flow step: the body of `CLiquidCell::Update`, full of `GetCellPtr` + neighbor tests + cell swaps |
| 2 | `F_00709960` | 9.9 | 8.5 | liquid "can move into this neighbor?" test: vcalls on the neighbor, an LCG roll (`rand%101 < 75`), a density compare (material +0x15c) |
| 3 | `F_00722cd0` | 5.5 | 4.7 | `GridWorld::RenderTiles` DrawGrid job body (cells → tile pixels) |
| 4 | `F_0070ae40` | 4.2 | 3.9 | `CLiquidCell::Update` (vtable slot 21) |
| 5 | `F_0089a100` | 3.2 | 3.2 | `Grid::GetCellPtr(x, y)` (512×512 chunk table; missing chunk → shared empty cell) |
| 6 | `F_00729ae0` | 3.6 | 3.3 | `GridWorldThreaded` slot 29: is (x, y) inside the update rect |
| 7 | `F_0070b360` | 1.1 | 3.2 | swap two cells in the grid (moves the pointers, updates the cell's x/y, wakes the neighbor) |
| 8 | `F_007288a0` | 3.6 | 3.7 | `GridWorldThreaded` chunk update job ("FullUpdate … box2d terrain"); calls every cell's update |
| 9 | `F_0070d020` | 1.5 | 2.4 | `ICell` slot 35 base: per-cell reaction/burn roll (`rand%10001`, material fields) |
| 10 | `F_00861e00` | 2.8 | 2.5 | a cell's collision flags byte (solid/burnable/…) for the Box2D terrain mesh |
| 11 | `F_004ac0f0` | 2.3 | 1.9 | `ICell` slot 12 getter: `return this->field_14` (2 instructions) |
| 12 | `F_00899f00` | 1.1 | 2.1 | `Grid::GetCellPtr(x, y)`, the variant that writes the empty-cell sentinel |
| 13 | `F_0070b5a0` | 1.2 | 0.9 | liquid random sideways drift (LCG, 1-in-3 left/right) |
| 14 | `F_00861e90` | 1.9 | 1.1 | Box2D terrain: marching-squares pass over 2×2 cell blocks (bit-count table at 0xfdeb30) |
| 15 | `F_00709810` | 1.3 | 1.1 | `CLiquidCell` slot 11: cell state query (0/1/3) from material flags |
| 16 | `F_0070d8c0` | 1.2 | 1.7 | cell slot 35 override for all cell classes: wraps `F_0070d020` |
| 17 | `F_00709350` | 0.4 | 0.9 | liquid helper: if (x, y) is in range and occupied, wake that cell (vcalls 0x14, 0x44) |
| 18 | `F_00708dd0` | 1.5 | 2.5 | `CGasCell::Update` (slot 21), more of it in heavy (explosions → smoke) |
| 19 | `F_0070e140` | 0.5 | 1.0 | gas/fire spread roll: `rand%101` vs the cell's chance, then a neighbor lookup |
| 20 | `F_007297d0` | 0.7 | 1.0 | `GridWorldThreaded` slot 5: grow the dirty rect to include (x, y) |

The top 20 cover 58% (flood) and 60% (heavy) of busy time. Every function in the list is part of the cell simulation, the Box2D
terrain rebuild or tile rendering. Box2D bodies, Lua and the entity systems don't appear.

## Job system (PLAN3 Phase 9)

Taken 2026-09-27 with `MINA4MAC_JOBLOG=<file>` (`runtime/joblog.c`) during perfbench, summarized with
`uv run tools/joblog.py <log> --framelog <perfbench_frames.txt>`. Logs and reports: `build/joblog/` (flood, heavy).
The log hooks every `std::function` call on guest threads (all jobs run through one), the main thread's wait loops
(polls of ConcRT's scheduler id), condvar waits and notifies, and records the core each job ran on
(`pthread_cpu_number_np`; 0–1 are the E-cores, checked with a background-QoS spinner). Logging costs about 4–6%
(flood work_ms 15.6 vs 14.8–15.0; heavy fps 33.3 vs ~35).

| | flood | heavy |
|---|---|---|
| frame (swap to swap) | 21.0 ms | 30.0 ms |
| main in job barriers | 8.6 ms (41%) | 14.5 ms (48%) |
| of which the chunk-update barrier (`0x726a5e`, 4 passes/frame) | 8.1 ms | 13.2 ms |
| per chunk pass: jobs, sum of job time, longest job | 28, 13.8 ms, 2.3 ms | 31, 27.0 ms, 3.5 ms |
| per chunk pass: main's wait, longest job still running when it started | 2.06 ms, 1.70 ms | 3.24 ms, 2.59 ms |
| worker utilization while main waits (9 threads) | 54% | 73% |
| jobs per frame, median job | 372, 21 µs | 425, 46 µs |
| workers busy | 9 × 33% | 9 × 47% |
| job time on E-cores | 14% | 13% |
| median job on P vs E | 17 vs 151 µs | 38 vs 252 µs |
| chunk passes whose last job ended on an E-core | 10% | 14% |
| wake latency (notify → worker running): median, p95 | 5, 24 µs | 6, 27 µs |

- **Pool:** GetSystemInfo reports 10 CPUs. The game starts 18 `std::thread`s: a job pool of 9 (= 10 − 1) that runs
  everything, and a second pool of 9 that runs about 1 job per frame. The main thread also runs some chunk updates
  inline (`0x726a37`) before it waits.
- **Why main waits:** nearly all of it is the 4 checkerboard passes of the chunk update (`F_00726a51`: submit the
  pass's chunks, wait until `pending == 0`, next pass). The wait is set by the pass's critical path: in flood, one
  chunk job (2.3 ms) is longer than the pass's ideal share (13.8 ms / 9 = 1.5 ms). In heavy, the pass is closer to
  throughput-bound (27 ms / 9 = 3.0 ms vs a 3.2 ms wait). The other ~15 barrier sites per frame cost < 1 ms together.
- **The wait is a spin:** `Sleep(0)` → `usleep(0)` returns in ~0.2 µs (42k–68k polls per frame), so main holds a
  core the whole time it waits. With 9 workers plus the spinning main on 10 cores, the workers land on the 2 E-cores
  for ~13% of job time, and jobs there are several times slower, so they often become the tail of a pass.
- **Not the cause:** wake-up latency (µs, against ms-long passes), main noticing late (it sees the last job end
  within one poll), and preemption inside jobs (thread CPU / wall 0.91–0.95).
- So the levers are: keep workers off the E-cores (QoS, or a pool of 7 = P-cores − main), stop main's spin from
  taking a P-core, and, most of all, shorten the chunk jobs themselves (Phases 11–12), since the passes' length is
  the per-cell code's speed.

## Scheduling (PLAN3 Phase 9)

Taken 2026-09-27 with `runtime/sched.c`'s env knobs (`MINA4MAC_CPUS`, `MINA4MAC_QOS`, `MINA4MAC_YIELD`) through
wrapper scripts in `build/sched/`, each run as `tools/perfab.sh <default> <variant> 3` (ABBA-interleaved, medians,
min–max in parentheses). Logs: `build/sched/{heavy,combo,h2h}.txt`, runs in `build/perfab/`. The baseline drifts
between batches (heavy 31.8–35.1 fps), so only compare A with B inside one row.

| B (vs the old default: 10 CPUs, no QoS, `usleep(0)`) | scene | fps | work_ms | main cpu_ms |
|---|---|---|---|---|
| QoS user-interactive | heavy | +4.1% | −1.0% | −7.2% |
| QoS user-initiated | heavy | +5.2% | −5.9% | −8.8% |
| report 8 CPUs (P-cores; pool of 7) | heavy | +0.4% | −2.0% | −1.2% |
| report 7 CPUs (pool of 6) | heavy | −5.3% | +3.9% | +5.4% |
| yield: `sched_yield` | heavy | +2.7% | −3.3% | −26% |
| yield: 64 × `yield` then `sched_yield` | heavy | +2.8% | −1.8% | −23% |
| yield: `usleep(20)` (nap20) | heavy | +3.7% | **−4.9%** (p95 −6.9%) | −57% |
| user-initiated + nap10 | heavy | +3.3% | −4.3% | −53% |
| user-initiated + nap20 | heavy | +3.1% | −2.9% | −55% |
| user-initiated + nap50 | heavy | +1.1% | −1.1% | −55% |
| user-initiated + nap20 | flood | −0.7% | −1.2% | −55% |
| head-to-head, 4 pairs: user-initiated + nap20 vs nap20 alone | heavy | +0.7% | −0.4% | +4.6% |

- **Kept: nap20** (the new default). Main's job wait (`_Thrd_yield` → `Sleep(0)`) now sleeps 20 µs per poll instead
  of spinning, so it stops holding a core. Heavy: ~5% less work per frame and less than half the main-thread CPU.
  Flood (swap-bound at 51 fps, lighter passes) is unchanged apart from the CPU. With joblog, the E-cores' share of job
  time fell from 13% to 10.4%, and passes that end on an E-core from 14% to 10% (`build/joblog/heavy_nap20_report.txt`).
  A longer nap (50 µs) gives back the gain, because main notices the end of each pass later.
- **Not kept:** QoS (nothing on top of nap20 in the head-to-head; its solo gain came from a batch with a slow
  baseline), a smaller pool (P-cores only doesn't help, 7 CPUs is worse: the game gets fewer workers), and the
  spinning yields (they help only as far as they give the core up, which the nap does better).
- **Sync HLE:** `runtime/sync.c` and the ConcRT condvars are plain pthread mutexes and condvars with no polling, and
  joblog measures wake latency at 5 µs median (p95 24–27 µs) against 2–3 ms passes. Nothing to fix there.
- What remains is the passes' critical path: the chunk jobs themselves (Phases 11–12).

## After Phase 11 (PLAN3 Phase 12 re-profile)

Taken 2026-09-28 at commit 87e7dcb (ThinLTO, fixed MEM, inline caches, live register sync), heavy scene, 20 s:
`build/perfprof/20260928-180220/`. The sampled run: 43.3 fps, work_ms median 17.39 (unsampled runs: ~47–48 fps,
~16.1 ms).

| bucket | main | guest threads | share | Phase 8 heavy share |
|---|---|---|---|---|
| lifted noita code | 0.21 | 3.53 | 88.1% | 78.6% |
| lifted msvcp120 | 0.00 | 0.01 | 0.3% | 0.3% |
| dispatch (`guest_call`, `call_thunk`) | 0.01 | 0.02 | 0.5% | 13.3% |
| math HLE | 0.00 | 0.02 | 0.5% | 0.3% |
| other HLE | 0.04 | 0.06 | 2.2% | 2.2% |
| Lua | 0.00 | 0.00 | 0.0% | 0.0% |
| GL bridge + driver | 0.03 | 0.00 | 0.7% | 0.7% |
| GL swap | 0.23 | 0.00 | 5.5% | 2.8% |
| FMOD | – | – | 2.0% | 1.7% |
| total busy | | | 4.24 cores | 4.95 cores |

- Dispatch is gone as a bucket. Busy cores fell (4.95 → 4.24) because the same work takes less CPU. The 9 workers are
  ~40% busy each, and the main thread is 52% busy (swap 23%, guest 21%).
- Top self time: `F_0070a520` 14.3% (liquid flow step), `F_00709960` 9.9%, `F_00722cd0` 5.9% (RenderTiles),
  `F_0070d020` 4.4%, `F_0070ae40` 4.2%, `F_007288a0` 4.1%, `F_0070b360` 3.7%, `F_00861e00` 3.5%, `F_00708dd0` 3.3%,
  `F_00708d00` 2.6%. The getters (`F_004ac0f0`, `F_0089a100`, `F_00899f00`, `F_00729ae0`) have left the list: ThinLTO
  inlines them into their callers through the inline caches and direct calls, so their cost now counts there.
- These functions contain almost no x87 (3 of ~6,000 instructions) and no `rep movs/stos`. Float math is scalar SSE.

## Jungle (PLAN4 Phase 15)

Taken 2026-09-28 at 931a724, `SCENE=jungle DELAY=15 SECS=30 tools/perfprof.sh` (the sample covers the slow second
half of the scene, frames ~900–2000): `build/perfprof/20260928-224207/`. The sampled run: 41.9 fps, work_ms 20.1
(unsampled: ~43–45 fps, 17–19 ms).

| bucket | main | guest threads (18) | share |
|---|---|---|---|
| lifted noita code | 0.35 | 1.18 | 69.3% |
| HLE | 0.15 | 0.06 | 9.4% |
| GL swap | 0.15 | – | 6.7% |
| FMOD | – | – | 5.9% |
| GL bridge + driver | 0.09 | – | 4.2% |
| dispatch / math HLE | 0.02 / 0.01 | 0.02 / 0.03 | 1.7% / 1.7% |
| Lua | 0.01 | 0.00 | 0.4% |
| total busy | 0.79 | | 2.22 cores |

Unlike flood and heavy, the job workers are mostly idle (the 9 cell-sim workers ~11% busy, the second pool of 9
~4%), and **the main thread is the bottleneck: 79% busy**. Its busy time (100%):

- World update, `DeathMatch[3]` (`F_006b26c0`) 62%:
  - Component systems (`F_0057a4c0`) 41%. Of that, `PhysicsBodySystem` (`F_00c608a0`) 15.6%, nearly all of it a
    **spin-wait for Box2D**: `F_00bd82c0` (physicsbody updator) loops `while (world->m_flags & e_locked || …)
    { Platform::Sleep(0.0); if (GetTime() - t0 > limit) log "Locked at: " }`. `Platform::GetTime` (`F_00dd9f30`,
    PlatformWin vtable slot 27, a QueryPerformanceCounter wrapper) is why `mach_absolute_time` shows up as 13.4% of
    main's busy time, filed under HLE. `PhysicsBody2System` and `PlayerCollisionSystem` have the same loop (~2.4%
    more). The other systems are a long tail (particle emitters 3.3%, sprite stains 2.4%, damage models 1.7%, …;
    none above 3.5%).
  - `F_006f04a0` → `F_00718e10` 17.8%: the main thread's part of the world/cell update (`F_007174b0` 7.4%,
    `F_00724a90` 4.0%).
- Render, `DeathMatch[4]` (`F_006b3ae0`) 16.5%, two-thirds of it in GL calls (`glDrawArrays` 5.4%).
- GL swap 19%.
- Waiting (not in the 100%): 21% of the thread's samples.

**Box2D's step runs on the second job pool** (`F_009a26d0` → `F_009beb10`, b2World::Step with its own QPC timers;
`F_009bdb40` has "Box2D - endless loop was broken"). One thread at a time runs it, for 31% of the wall time: about
9 ms of a ~29 ms sampled frame. Main waits for it about 4 ms a frame. So the frame's critical path is main's own
work plus the tail of the single-threaded physics step.

- **Box2D is double-precision scalar SSE**, and the lifter keeps XMM registers in `c->xmm[]`. clang doesn't promote
  them: `F_009b2ec0` compiles every `mulsd` to `ldr d; fmul; str d` on the CPU struct (build/asm_9b2ec0.txt), since
  guest-memory stores through the fixed `MEM` address may alias it. 85% of Box2D's self time is in functions with
  ≥20% XMM instructions; main thread 31% (+10% with 5–20%); cell-sim workers 12% (their hot code is integer).
  This is the XMM analogue of PLAN3's register sync (GPRs in locals, −18% heavy).
- **Scheduling**: `MINA4MAC_QOS=interactive` (runtime/sched.c, off since PLAN3 Phase 9 found nothing on heavy; the
  default since PLAN4 Phase 16, uncapped flood neutral)
  gives jungle work_ms **−8.2%** (18.1 → 16.6, fps +4.7%, p95 −10.6%, ranges disjoint, build/perfab_jungle_qos.log).
  The single-thread critical path (Box2D, main) is what an E-core hurts most.
- Ruled out by this profile: the Lua bridge (0.4%), dispatch/uncached indirect calls (1.7% in all; call_thunk 1.7%).

## Jungle after XMM in locals (PLAN4 Phase 16 re-profile)

Taken 2026-09-29 at 05643cd, same settings as above: `build/perfprof/20260929-094221/`. The sampled run: 46.8 fps,
work_ms 16.6 (fps per 300 frames 58.6 58.0 50.4 41.6 39.9 40.0). A regsum experiment (~3 cores for 85 s) overlapped
part of the sample, so treat shares as ±a few points.

| bucket | main | guest threads (18) | share | before (Phase 15) |
|---|---|---|---|---|
| lifted noita code | 0.37 | 0.93 | 64.3% | 69.3% |
| HLE | 0.08 | 0.06 | 6.8% | 9.4% |
| GL swap | 0.25 | – | 12.4% | 6.7% |
| GL bridge + driver | 0.09 | – | 4.8% | 4.2% |
| dispatch / math HLE | 0.01 / 0.02 | 0.01 / 0.02 | 1.3% / 1.8% | 1.7% / 1.7% |
| Lua | 0.01 | 0.00 | 0.3% | 0.4% |
| total busy | 0.84 | | 2.02 cores | 2.22 cores |

Main thread, % of its busy samples (scratch script over perfprof.parse; before → after):

- **The Box2D spin-wait is gone**: `PhysicsBodySystem` (`F_00c608a0`) 15.6% → 3.4%, `mach_absolute_time` 13.4% →
  2.3%. Box2D's step (`F_009beb10`) is 9.8% of all busy samples (was ~31% of wall time on one thread), so it now
  finishes before main gets there.
- World update (`F_006b26c0`) 62% → 53%: component systems 41% → 30%, now a flat tail (the top system is
  `F_00c72920` at 4.1%, then `PhysicsBodySystem` 3.4%, none of the rest above 2%); main's part of the cell update
  (`F_006f04a0`) 17.8% → 19.5%.
- Render (`F_006b3ae0`) 16.5% → 14.3%. Swap 19% → 30%: capped, a frame that misses a vsync waits in the swap, so this
  is mostly waiting that `sample` sees as busy.
- So main's own work is a long tail of ordinary lifted code with no single subsystem left to target. What's left in
  Phase 16 is lifted-code speed across the board (vcall reloads, stack slots) and main-thread parallelism.
