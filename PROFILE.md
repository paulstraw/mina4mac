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
