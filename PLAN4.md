# mina4mac, part 4: busy scenes (mobs and chaos)

This continues `PLAN3.md`, which is complete. Its benchmarks (flood and heavy liquid scenes) now run at 60 fps and 48 fps,
and a ~22-minute user playtest of `build/Noita.app` held 60 fps until the Underground Jungle, where it dipped to ~40.
**Read the Context sections of `PLAN.md`, `PLAN2.md` and `PLAN3.md` first.** Their rules (bring-your-own exe, never
commit game or FMOD files, `tools/check.sh` before every commit, record results in the task notes, interleaved A/B
with medians, correctness gates before measuring) all still apply.

The goal: find out what makes the jungle (and busy, mob-heavy scenes in general) slow, and make it hold 60 fps.

## Context

### Where PLAN3 stopped (2026-09-28)

- Final numbers (build/final/, PLAN3 Phase 13), capped at 60 Hz: flood 59.8 fps (Wine 55.9), heavy 47.9 fps (Wine 38.6,
  PLAN2 34.2). Uncapped: flood 86 fps, heavy 42 fps. Heavy frame work is 16.2 ms (p95 21.4 ms) against a 16.7 ms budget.
- What got there: register sync (−18% heavy work_ms), inline caches (−12%), a napping job wait (−5%), ThinLTO (−3.5%),
  guest memory at a fixed host address (−2%).
- Profile after PLAN3 (PROFILE.md "After Phase 11"): lifted noita code is 88% of busy samples, dispatch 0.5%, HLE 2.7%,
  Lua 0.0%, GL outside the swap 0.7%. Almost all of it is the falling-sand cell simulation on the 9 job workers.
- Ruled out by that profile: x87 in locals (no x87 in hot code), SSE/memcpy paths, the Lua and GL bridges, wide
  (non-wrapping) addressing (unsafe upper bound: −0.7%, noise).
- Left open: the lifted code's structural costs (guest stack slots and arguments live in guest memory; vcalls still
  reload ebx/esi/edi/ebp because vcall targets have no register summary).

### The playtest (2026-09-28, PLAN3 Phase 13)

- `tools/package_app.sh --no-build --fps` build; the fps log is `~/Library/Logs/mina4mac.log` (one `[fps]` line a second:
  fps, average and worst frame time).
- The user: "really fantastic perf until jungle, where it just tanked (~40fps)", with "a fair amount of mobs and
  general chaos" at the time.
- The log: median 60 fps in every minute of the run. Before the jungle, only isolated slow seconds. In the jungle, two
  dips of ~15 s each: at 20:24 into the run (42–57 fps) and 21:40 (38–52 fps), each recovering to 60.

### What we don't know yet

- **Neither benchmark scene covers this.** Both are liquid floods around a protected player with no enemies. Mobs,
  their AI, projectiles, Lua scripts, pathfinding, particles and Box2D bodies barely appear in them. The Lua bucket
  was 0.0%.
- **Whether it's us or the game.** The jungle may be just as heavy under Wine or on Windows. Only a Wine comparison
  of the same scene tells us whether the recompiler loses more there than elsewhere.
- **Which thread.** The cell simulation runs on the job workers. Entity systems, Lua and physics most likely run on the
  main thread, which PLAN3's profiles show as mostly idle (waiting for jobs) in the benchmark scenes.
- **Where the log points.** The fps log has no biome or entity counts, so a user report can only be matched by time.

### Facts that point at hypotheses

- **The inline-cache profile only saw flood and heavy.** `tools/icache_sites.txt` holds the 321 hottest indirect-call
  sites from those two scenes. The entity/component systems, AI and Lua-called C++ are full of vcalls, and sites that
  are hot in the jungle may have no cache at all: they pay for `guest_call` (13% of busy time before PLAN3's caches).
- **Lua goes through the host bridge.** Every `lua_*` call from guest code crosses `runtime/lua51.c`, and every Lua →
  C++ API call (`EntityGetTransform`, `ComponentGetValue2`…) calls back into guest code through `call_guest`. Mob
  AI and projectile scripts make many such calls per frame; PLAN3 never measured them under load.
- **Register sync is weakest at vcalls.** After a vcall, ebx/esi/edi/ebp are reloaded, since vcall targets have no
  summary. Component systems are vcall-heavy.

### Key design decisions

- **Reproduce before fixing.** A seed-pinned jungle scene in the perfbench mod comes first, confirmed against the
  playtest's numbers (a clear dip below the flood scene) and run under Wine too.
- **Profile before guessing.** The hypotheses above are ranked by a profile of that scene, per thread and per
  bucket, not by what was cheapest last time.
- **Reuse PLAN3's tools**: perfbench/perfab/perfprof, the `--icprof` counting build, `--sync check`, joblog,
  determinism. Extend them rather than writing new ones.

## Tasks

### Phase 14: reproduce it

- [x] A jungle benchmark scene (`SCENE=jungle` in `tools/perfbench/init.lua`).
  - Biome map row 27 (`data/biome_impl/biome_map.png`, 512 px a pixel, x0 = column 35, y0 = row 14) is
    `rainforest.xml` from x −2560 to 2047, y 6656–7167. The scene teleports the protected player to (−768, 6912)
    at frame 60, moves it to the nearest open spot at 180 (−720, 6928), logs `BiomeMapGetName` there and writes
    `fail …` (perfbench.sh exits 1) unless it is `$biome_rainforest`. Enemy waves from `rainforest.lua`'s list: 30
    at frame 200, then 12 more every 300 frames; `circle_fire` and a TNT box on alternating sides every 150. It
    measures frames 360–2160. The per-300-frame lines now carry the entities and `enemy`-tagged entities within
    1024 px (all scenes).
  - First try (18 enemies, then 9 every 600 frames, fire every 300): 58.0, 47.7, then 3 more runs at 57–58.5 fps,
    work_ms 13.7–14.3 ms. It only dipped for one 5 s window in some runs, so not busy enough (the user, watching:
    "way better than the perf I was seeing").
  - Crossing the jungle row at 2 px/frame (streaming in new world, same waves) was lighter still: 58.5 fps, work_ms
    13.1, enemies near the player falling 67 → 22 as it left them behind. World streaming isn't the dip; not kept.
  - Kept version, 2 runs (build/jungle_b1.txt, b2): fps per 300 frames 57.5 56.5 49.3 41.1 39.8 35.7 and
    57.6 54.1 41.1 41.1 38.1 37.4 (the playtest: 38–57); work_ms median 16.6 / 18.0 (p95 23.2 / 24.5), cpu_ms
    14.2 / 15.3. Enemies within 1024 px grow 88 → 138, entities 393 → 663. It gets slower as the enemies pile up,
    so the later windows are the ones to watch. Unlike heavy (capped: work_ms 16.2, cpu_ms 8.4), main-thread
    cpu_ms is close to work_ms, so the main thread's own work, not the job wait, looks like the cost (Phase 15 will
    tell).
  - Pin the seed as the other scenes do, and teleport the (protected) player into the Underground Jungle. Find the
    coordinates for the pinned seed (the biome map, or `BiomeMapGetName` at the target point) and log the biome name
    at the start, so a wrong spot fails loudly instead of benchmarking the wrong place.
  - Make it busy the way the playtest was: spawn a fixed set of jungle enemies around the player (and whatever
    spawns naturally), plus some fire or explosions at fixed frames. Everything happens at fixed frame numbers, so
    runs stay comparable.
  - Check it: the fps per 300 frames should sit clearly below 60 (the playtest dipped to 38–57). If it doesn't, make
    it busier before going on; a scene that doesn't reproduce the dip can't measure a fix.
- [x] Wine comparison: 3 rotated runs each of the current build and the Sikarugir Wine build, capped (as in PLAN3
  Phase 13). This answers "is it us or the game". If Wine is as slow or slower, the target is still 60, but the
  work is ordinary optimization, not a recompiler-specific gap.
  - 2026-09-28, SCENE=jungle, capped, rounds now → Wine, Wine → now, now → Wine. **now** = build/mina4mac at 158493f
    (copied to build/jungle_wine/now); **Wine** = the Sikarugir wrapper (perfbench now installed there too). Both
    reached `$biome_rainforest` in every run. Runs, logs and `run.sh`: build/jungle_wine/.

    | build | fps, median (min–max) | fps per 300 frames (median run) | last 300 frames | work_ms (p95) | main cpu_ms |
    |---|---|---|---|---|---|
    | now | **43.3** (42.7–45.6) | 58.3 57.1 47.0 38.2 37.7 33.5 | 38.4 (33.5–40.0) | 19.1 (24.4), 17.6–19.2 | 16.5, 15.1–16.6 |
    | Wine | 29.3 (29.2–29.8) | 35.5 33.5 29.1 27.9 29.4 23.3 | 23.3 (22.5–28.0) | – | – |

  - **It's the game, not us.** Native is +48% over Wine here, twice the heavy scene's gap (+24%). Wine never gets
    near 60 even in the first window (35–37 fps, native 58–59), so the jungle is simply a heavier scene. The
    recompiler doesn't lose more here than elsewhere; it gains more. What's left is ordinary optimization toward 60.
  - Both builds slow down the same way as enemies pile up (entities 580–720, enemies 117–139 within 1024 px at the
    end, similar for both).
  - Main-thread cpu_ms is 86% of work_ms (heavy capped: 8.4 of 16.2, 52%), so the main thread's own work, not the
    job wait, is where the frame goes. Phase 15 should start from the main thread's profile.
  - The scene's first two runs (45.2 and 43.6 fps, work_ms 16.6 / 18.0) fall inside this batch's range, so the
    scene is repeatable to about ±4% fps; compare fixes only inside one interleaved batch, as before.
- [ ] Playtest telemetry (moved after Phase 15 on 2026-09-28: the profile decides what comes next; this only has
  to be done before the next playtest): make it cheap to locate the next report. Either the fps line also carries the biome and
  entity count (from a tiny always-on mod writing to a file, or from guest state), or a separate `--telemetry`
  packaging option. Keep it off the hot path (once a second).

### Phase 15: find the cost

- [x] Profile the jungle scene with `tools/perfprof.sh` (`SCENE=jungle`) and put it next to PROFILE.md's heavy
  profile: buckets per thread (main vs job workers), top guest functions by self and inclusive time, and what the
  main thread does between swaps. Name the top 20 functions with `tools/fninfo.py`, as in PLAN3 Phase 8.
  - 2026-09-28, PROFILE.md "Jungle": `build/perfprof/20260928-224207/` (30 s over the slow second half). **The main
    thread is the bottleneck** (79% busy; the 18 workers 4–11% each). Its busy time: component systems 41% (of which
    15.6% is PhysicsBodySystem *spin-waiting* for Box2D's step, polling `Platform::GetTime`, hence
    `mach_absolute_time` at 13% of main as "HLE"), main's part of the world update 18%, render 16.5% (2/3 GL
    calls), swap 19%. The rest of the systems is a long tail (none above 3.5%).
  - Box2D's `b2World::Step` runs on the second job pool, one thread at a time, ~31% of wall time (~9 ms of a sampled
    frame); main waits ~4 ms a frame for it. Box2D is double-precision scalar SSE, and the lifter keeps XMM in
    `c->xmm[]`, which clang loads and stores around every SSE op (build/asm_9b2ec0.txt). XMM-dense functions (≥20%)
    are 85% of Box2D's self time and ~31% of main's.
  - `MINA4MAC_QOS=interactive`: jungle work_ms −8.2% (18.1 → 16.6, fps 44.0 → 46.0, disjoint ranges), heavy +0.4%
    (noise; 48.2 vs 48.1 fps). build/perfab_{jungle,heavy}_qos.log.
  - Lua is 0.4% of busy samples and dispatch 1.7%, so the next two tasks have little to find (see Phase 16).
- [ ] Count what the benchmarks didn't: Lua bridge calls and guest callbacks per frame (a counter build or a
  `MINA4MAC_*` knob in `runtime/lua51.c`), and the cost per call. Entities alive per frame, from the mod.
- [x] Indirect calls in the jungle: run the `--icprof` counting build on the jungle scene and compare with the
  flood/heavy profile. How many of its hot sites are uncached, and what share of its indirect calls miss a cache?
  - 2026-09-29 (done with the Phase 16 inline-cache task, build/icj/): 5.0e9 indirect calls over 23.1k sites (heavy
    1.6e10 over 21.7k), still almost all monomorphic (the top 400 sites take 95.9%, their top target 97.7%). But
    they're spread wider: 796 sites reach 99% (heavy 365), 419 of those had no cache (5.2% of calls), and with the
    flood/heavy sites only 93.1% of jungle calls hit a cached target (heavy 98.0%). The counting build ran the scene
    at 22 fps; `perfbench.sh` now takes `TIMEOUT=<s>` for slow builds (default 300).

### Phase 16: fix what the profile shows

The order below is a guess. Re-rank it from Phase 15 before starting, and drop any task the profile doesn't support.

Re-ranked 2026-09-28 from the jungle profile (PROFILE.md "Jungle"): first the two new tasks below, then the vcall
summaries and stack slots (both lifted-code speed, which is still most of main's time). Inline caches and the Lua
bridge are unlikely to pay (dispatch 1.7%, Lua 0.4%); keep them only as a cheap check after the others.

- [x] QoS user-interactive by default (`runtime/sched.c`): measured jungle −8.2% work_ms, heavy neutral. Confirm
  flood holds 60, then flip the default and keep the knob.
  - 2026-09-28: flood capped holds 60 (59.8 vs 59.7 fps), but its capped work_ms rose +20% (10.65 → 12.81, cpu_ms
    +15%, ranges disjoint; build/perfab_flood_qos.log). Uncapped flood shows no real loss: fps −1.2%, work_ms +1.4%,
    ranges overlapping (build/perfab_flood_unc_qos.log). So the capped rise is idle headroom under the 60 Hz cap
    (clocks or placement at light load), not lost throughput; heavy (+0.4%) and jungle (−8.2%) are the loaded cases.
  - Now the default. `MINA4MAC_QOS=interactive|initiated|none` (`none` = the old unspecified default) stays as the
    knob; the build/sched/base wrapper now needs `MINA4MAC_QOS=none` to reproduce the old behaviour.
  - check.sh: all steps ok except one difftest trial (seed 16051): `0x87ae40` (a noise function, float args in
    xmm0–2, keeps values in xmm5–7 across its calls, so an LTCG convention) mismatches on xmm5/6/7 and one stack byte
    in ~25% of trials (`difftest.py --all --only 0x87ae40 --trials 20`: 6/20 and 5/20 fails on two seeds). Not this
    change: difftest doesn't link runtime/sched.c, and check.sh just happened to sample it. The mismatched registers
    are plain `movapd`/`movaps` copies of the inputs, so the first suspect is NaN handling on XMM moves. Investigate
    at the start of the XMM task below, since that task rewrites exactly this code.
- [x] XMM registers in C locals, like PLAN3's GPR register sync: load the XMM registers a function uses at entry,
  keep them in locals, store dirty ones before calls/returns/exits and reload after calls. MSVC's x86 convention
  treats all XMM as volatile, but check for LTCG custom conventions passing values in XMM (a `--sync check`-style
  mode). Target: Box2D's step (the tail main spin-waits on) and main's SSE code. A/B on jungle and heavy.
  - 2026-09-29, the 0x87ae40 mismatch first: not a lifter bug. Unicorn traced it to `addss` of inf + −inf at
    0x870b4c (random inputs overflow the noise function), which makes the *default NaN*: x86's is negative
    (0xffc00000), ARM's positive (0x7fc00000). The NaN then flows into xmm5–7 and two stack slots. Emulating the
    x86 sign would cost every float op, and the determinism check never saw it, so `difftest.py` now compares
    with the x86 default NaN mapped to ARM's (`canon_nan`, aligned f32 and f64 words): 40/40 trials pass on two
    seeds.
  - `tools/lift.py`: the XMM registers a function names live in locals `x0`–`x7`, loaded at entry. Store markers
    also store the XMM locals assigned since the last sync (the same dirty analysis; `XMM_WRITE_RE`), and every
    call reloads all of the function's XMM locals. So no summaries and no check mode are needed: it's as safe as
    before for any convention, including 0x87ae40's LTCG one (xmm5–7 kept across its calls). The loose-instruction
    tools use the new `FnLifter.prologue()`.
  - New `difftest.py --sse` (functions whose own body names an XMM register), now a check.sh step. 695 such
    functions: 505 + 1026 trials pass (seeds 11, 23), 0 fail; default difftest 2231 pass, 0 fail.
  - Box2D's `F_009b2ec0`: 351 → 281 lines of asm, loads/stores 207 → 108, no 128-bit q loads/stores left.
  - A/B, 3 interleaved pairs each, both sides `MINA4MAC_QOS=interactive` (build/xmm/, build/perfab/20260929-*):

    | scene | fps A → B | work_ms A → B | work_p95 | main cpu_ms |
    |---|---|---|---|---|
    | jungle | 42.6 → **46.4 (+9.1%)**, ranges disjoint | 19.0 → 16.8 (−11.9%) | −16.5% | −14.2% |
    | heavy | 47.3 → 48.6 (+2.8%) | 16.3 → 15.9 (−2.8%) | +0.6% | −5.0% |

  - check.sh ok (the new sse difftest step: 515 pass; the launcher step hit the known agent-launch stall).
    Determinism (build/xmm/seedprint_*.txt): A vs B seed/procedural/random/libm identical, snapshots f60 61–63 and
    f600 43–47 lines against a B-vs-B band of 50 / 42. Against Wine, A reads f60 89 and B 80–82 today, so those
    counts are noise, not this change.
  - Left for later if the next profile asks for it: skipping XMM reloads after calls to functions that provably
    don't write XMM (a regsum-style summary). Every call currently reloads each XMM local it uses.
- [x] Inline caches for all three scenes: merge the jungle profile into `tools/icache_sites.txt` (`tools/icache.py`
  already merges scenes with equal weight) and A/B on jungle *and* heavy, so the new sites don't cost the old scenes.
  - 2026-09-29: `tools/icache.py build/icprof/heavy.tsv build/icprof/flood.tsv build/icj/jungle.tsv` (same settings):
    388 → 607 sites (321 → 490 caches; the rest go to import thunks). Hits: jungle 93.1% → 97.0%, heavy 98.0% →
    98.2%, flood 98.7% → 99.1%. Binary unchanged at 61.4 MB.
  - A/B, A = 64137c9 (build/icj/, build/perfab/20260929-110010 and -110728):

    | scene | pairs | fps A → B | work_ms A → B | work_p95 | main cpu_ms |
    |---|---|---|---|---|---|
    | jungle | 4 | 46.6 → 48.0 (+2.9%) | 16.55 → 15.96 (−3.6%), B faster in 3 of 4 pairs | +0.1% | −3.9% |
    | heavy | 3 | 47.7 → 48.2 (+1.1%) | 16.15 → 15.94 (−1.3%) | +0.4% | −4.2%, ranges disjoint |

    Small but in the expected direction (about 4% of the jungle's indirect calls stop going through `guest_call`),
    and heavy doesn't lose. Kept.
  - check.sh ok (seed 29486; the launcher ran its 300 s into a world). Determinism not rerun: only which sites get a
    cache changed, and a cache miss still goes through `guest_call` to the same target.
- [ ] Lua bridge overhead, if Phase 15 shows it: shortcut the hot `lua_*` entry points, avoid per-call setup in
  `call_guest` callbacks, check that LuaJIT's JIT is on for the game's scripts and isn't aborting traces on the
  bridge's C functions.
- [x] Register summaries for vcall targets: where an inline cache or a constant vtable names the target, use its
  regsum summary, so a vcall stops reloading ebx/esi/edi/ebp. Verify with `build_all.py --sync check` on all scenes.
  - 2026-09-29, re-profile first (PROFILE.md "Jungle after XMM in locals", `build/perfprof/20260929-094221/`): the
    Box2D spin-wait is gone (PhysicsBodySystem 15.6% → 3.4% of main's busy time, `mach_absolute_time` 13.4% → 2.3%).
    Main's work is now a flat tail of lifted code (systems 30%, none above 4%; cell update 19.5%; render 14%), plus
    swap 30% (capped, mostly waiting for the next vsync). Dispatch 1.3%, Lua 0.3%. So the general lifted-code items
    are what's left, and this one first.
  - Inline-cached branches already reload by their target's summary (PLAN3), so the task was the `guest_call`
    fallback and regsum's own analysis, where an indirect call made ebx/esi/edi/ebp unknown. That also weakened the
    summary of every function that calls a vcall without saving those registers itself, and so its callers' reloads.
  - A whole-program proof doesn't go through: regsum proves all four preserved for only 70% of the 83k address-taken
    functions (78% if indirect calls preserved them), and most failures are precision (e.g. static initializers whose
    `atexit` chain it can't see into), not real violations. So it's the calling convention instead:
    `regsum.INDIRECT`, used for non-IAT indirect calls in regsum and in the lifter's reload. MSVC only gives a
    function a custom (LTCG) convention when it sees all its call sites, so an address-taken function preserves
    ebx/esi/edi/ebp; host thunks and msvcp120 exports do too, and the EH runtime (funclets with a borrowed ebp) is
    host code. `MINA4MAC_VCALL=unknown` at build time restores the old behaviour.
  - `build_all.py --sync check` build (checks at 85k of the 100k `guest_call` sites): jungle, heavy and flood all ran
    to the end without a sync failure (build/vcall/check_*.txt).
  - Summaries: preserving 4/4 64,648 → 68,136 functions, pops known 68,285 → 72,737. Reloads per call return 5.74 →
    4.98 (−13%); binary 62.8 → 61.4 MB.
  - A/B (build/vcall/, build/perfab/20260929-09*, -10*), A = 05643cd:

    | scene | pairs | fps A → B | work_ms A → B | main cpu_ms |
    |---|---|---|---|---|
    | heavy | 3 | 47.2 → 49.0 (+3.8%) | 16.28 → 15.73 (−3.4%, ranges overlap) | −5.3%, ranges disjoint |
    | jungle | 3 + 4 | 46.4 → 48.0, then 46.9 → 45.8 | 16.45 → 16.64 over all 7 (noise) | 13.97 → 14.38 (noise) |

    Jungle's run-to-run spread (±5%) is larger than the effect. Kept anyway: heavy's main thread gains clearly, the
    change is a few lines, it removes code, and the check build found no violation.
  - check.sh ok (seed 9483; the launcher step hit the known agent-launch stall). Determinism not rerun: the change
    only skips reloads, and the check build compares every skipped one at run time.
- [ ] Main-thread parallelism, if the main thread is the bottleneck while the job workers idle: check whether the
  game has work it could hand to its job system that the recompiled build serializes (a scheduling or HLE effect,
  as in PLAN3 Phase 9), not changes to the game's own design.
- [ ] Carried over from PLAN3 Phase 12: stack slots and arguments in C locals (a per-frame escape analysis like
  regsum.py's, plus a check mode). The largest and riskiest item: only if the profile shows the lifted code's memory
  traffic, not a specific subsystem, as the cost.

### Phase 17: wrap up

- [ ] Final numbers for all three scenes (flood, heavy, jungle), native vs Wine, capped, with work_ms and cpu_ms, as in
  PLAN3 Phase 13. Record what each kept change contributed.
- [ ] Rebuild `build/Noita.app` (with `--fps`, plus telemetry if Phase 14 added it) and have the user play through the
  jungle again. Match any dips against the log.
