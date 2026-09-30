# mina4mac, part 5: the snow-level slowdown

This continues `PLAN4.md`. Its benchmarks cover liquid floods (flood, heavy) and a mob-heavy jungle. The second
telemetry playtest (PLAN4 Phase 17, 2026-09-29) found a slowdown that none of them look like: a steady 48 fps in an
ordinary snow level with few enemies around, after a fight. **Read the Context sections of `PLAN.md` through
`PLAN4.md` first.** Their rules (bring-your-own exe, never commit game or FMOD files, `tools/check.sh` before every
commit, record results in the task notes, interleaved A/B with medians, correctness gates before measuring) all still
apply.

The goal: find a scenario that reproduces the dip, find out what costs the time, and make it hold 60 fps if the
cost is ours to cut.

## Context

### The report (2026-09-29, second telemetry session)

- Build: `tools/package_app.sh --telemetry` at 476b7f4. Log: `~/Library/Logs/mina4mac.log` from that launch (it
  becomes `mina4mac.log.1` on the next launch, then is gone; copied to `build/playtest2/mina4mac.log`, gitignored).
- What the user did: made Steve (the Holy Mountain shopkeeper) angry in the Holy Mountain below snowcave (y ≈ 4980),
  escaped back up into snowcave with a black hole, then fought "a couple normal snow level enemies". "It got slow
  even once they were dead." They went back down into the Holy Mountain, reached the Hiisi Base and died later.
- What the log shows, frames 83280–84420 of that run:
  - 25 s at **a flat 48.0 fps**: frame avg 20.84 ms in almost every second, worst frame 21–28 ms. No spikes: a
    steady cost just over the 16.7 ms budget, not hitches or loading.
  - Snowcave, x 270–610, y 4120–4620. **Entities 130–180, enemies 20–34** within 1024 px, fewer than scenes that
    ran at 60 (snowcastle: 400+ entities, 100+ enemies at 55–60 fps).
  - Location-bound: it ended within a second or two of entering the Holy Mountain (60 fps again). The same spot
    (578,4210) ran at 60 fps ~16000 frames earlier in the run.
  - Between the Holy Mountain and the dip there are stretches of "no player" (no `player_unit` entity: frames
    72060–72600, 79440–79800) while the game kept running at 60. The user confirms they were polymorphed for a
    while. The telemetry loses the player then (it looks for `player_unit`); scenarios must not depend on it.
  - The session's `Setting random seed:` line (1440576598) is the only seed in the log. It may belong to the last
    run (the one with the dip) or to the first; check before relying on it.
- Other dips in the same session, for comparison: fungicave ~20 s at 49–56 fps (80–90 enemies), Holy Mountain
  arrivals 44–55 fps for 1–2 s, deaths and new-game loading 25–40 fps.

### What we don't know yet

- **What was running.** The telemetry counts entities and enemies only. Fire, liquids, gas, loose falling
  material, physics bodies (ragdolls, debris) and particles don't show up in it, and it doesn't say whether main or
  the job workers were busy.
- **Whether it's us or the game.** As with the jungle (Wine 31 fps vs native 46), the scene may be just as heavy
  under Wine. Only a Wine run of a reproducing scenario answers that.
- **Why exactly 48.0.** The heavy benchmark also sits at 48 fps capped, and a steady 20.84 ms per frame with a
  worst of ~21 ms isn't a mix of 16.7 and 33.3 ms vsync intervals (that would show a worst near 33). The display
  is a 60 Hz 5K external. Either the frame really is ~20.8 ms of work every frame, or something in the swap/pacing
  path quantizes to it. Check this before optimizing toward a number that might be a pacing artifact.

### Hypotheses, to be ranked by the scenarios

1. **Leftovers of the fight in the cell simulation (job workers).** Fire spreading through snowcave's wood and
   fungus, burning liquids, steam and smoke, snow/ice melting to water. Steady and location-bound, like the
   report. Mob counts wouldn't show it.
2. **The black hole's tunnel.** Unsupported material around the tunnel collapsing and falling for a long time, or
   rigid bodies cut loose from the terrain (Box2D).
3. **Physics bodies from the fight.** Ragdolls, gibs, frozen bodies, debris piling up near the player.
4. **The angered Holy Mountain or the polymorph.** Steve's state, anything the anger spawns or keeps alive, or
   something the polymorph left behind. Least likely: the polymorph periods themselves ran at 60, and the dip
   ended on entering the Holy Mountain.
5. **Pacing, not work.** See "Why exactly 48.0".

### Key design decisions

- **Measure the real thing first.** Before building scenarios, make the next playtest self-diagnosing
  (per-second main-thread work/cpu in the log, a one-command live sample of the running app). A single live profile
  of the next occurrence is worth more than a dozen guessed scenes.
- **Scenarios are a sweep, not a single scene.** One perfbench scene per hypothesis, all in snowcave with the same
  pinned seed, same spot, same fixed frames, so they compare directly with each other and with a quiet baseline.
- **Try the playtest's own world.** If the seed checks out, pin it and teleport to the report's coordinates: same
  terrain and material layout as the dip.
- **Reuse PLAN3/PLAN4's tools** (perfbench/perfab/perfprof, joblog, framelog, telemetry). Extend them rather than
  writing new ones.

## Tasks

### Phase 18: better eyes on playtests

- [x] Per-second main-thread split in the `[fps]` line of the packaged app: the median `work_ms` and `cpu_ms` of
  the second (the numbers `MINA4MAC_FRAMELOG` writes per frame, aggregated in `runtime/sdl2.c`, no file I/O).
  cpu close to work = main's own work (jungle-like); cpu well below work = main waits for the job workers
  (cell-simulation-like). Only with `--fps`/`--telemetry`; check the flood scene's fps is unchanged.
  - 2026-09-29: averages rather than medians (no per-frame storage): `main work 12.11 ms, cpu 5.05 ms` after the
    frame times. Two clock reads per frame, only when `MINA4MAC_FPS` or `MINA4MAC_FRAMELOG` is set, so perfbench
    (no `MINA4MAC_FPS`) and plain play are unchanged; no flood run needed. At the start area of a new game: work
    12–13 ms, cpu ~5 ms, so main waits ~7 ms a frame for the workers there.
- [x] More telemetry counters, still once a second: physics bodies near the player (entities with a
  `PhysicsBodyComponent` or `PhysicsBody2Component`, or a cheap tag-based stand-in), projectiles (`projectile`
  tag), and the polymorphed player (find it via the `polymorphed_player` tag or similar, so the position and
  counts keep working instead of "no player"). Look for a cheap Lua-visible count of burning or moving cells; if
  there's none, say so and skip it.
  - 2026-09-29: `bodies` (entities within 1024 px with a `PhysicsBodyComponent` or `PhysicsBody2Component`; loose
    terrain cut into rigid bodies isn't an entity, so it doesn't show), `projectiles` (tag), and `poly` after the
    position when the player is found by `polymorphed_player` (not tested in a polymorph yet). No Lua-visible count
    of burning or moving cells found; skipped. Start area: `entities 290, enemies 47, bodies 38, projectiles 0`.
- [x] `tools/playprof.sh`: sample the running `Noita.app` (its `mina4mac` binary) for 20 s and summarize with
  `tools/perfprof.py` into `build/playprof/<time>/`, so "it's slow right now" becomes a profile without stopping
  the game. Check that perfprof.py's buckets work against the app binary (symbols, path).
  - 2026-09-29: samples the newest `mina4mac` process, keeps the `[fps]` lines logged meanwhile (`fps.txt`), then
    runs perfprof.py. Tested on the packaged app in a new game: buckets and threads come out as for perfbench runs
    (build/playprof/20260929-224813). Sampling every 5 ms costs ~10% fps while it runs (60 → 53); the `[fps]`
    lines show it, so read them with that in mind.
- [x] Log retention: the launcher keeps two launches. Keep more (e.g. the last 5), or have the packager print a
  reminder, so a playtest isn't lost to a relaunch.
  - 2026-09-29: the launcher keeps `mina4mac.log.1` (newest) to `.5`; README updated. check.sh ok (seed 9031).

### Phase 19: the scenario sweep

All scenes: pinned seed, protected player teleported into snowcave (check `$biome_snowcave` at the spot, fail
loudly otherwise, as the jungle scene does), a settle period, then 1800 measured frames with the per-300-frame fps,
entity and enemy counts. Each scene is a separate `SCENE=snow_*`.

- [ ] `snow_quiet`: nothing happens. The baseline; it should hold 60 fps. If it doesn't, the snow level itself is
  the finding.
- [ ] `snow_fight`: spawn a handful of snowcave enemies (from the biome's own spawn list) near the player and kill
  them after a few seconds (damage them from the mod), then measure the aftermath with no living enemies. Covers
  ragdolls, gibs, blood, frozen bodies (hypothesis 3).
- [ ] `snow_fire`: set fires in snowcave's wood and fungus at fixed frames and measure while they burn and after
  (hypothesis 1). Include melting (fire and lava against snow and ice) and steam.
- [ ] `snow_blackhole`: fire black holes upward through the terrain from below the player's area, as the escape
  did, then measure the tunnel's aftermath (hypothesis 2).
- [ ] `snow_combo`: black hole tunnel, then the fight, then fire, in that order: the playtest's sequence as closely
  as the mod can script it.
- [ ] `snow_steve`: anger the Holy Mountain below (the game's own anger path, e.g. the global flag the shop uses, or
  damage to Steve), let him chase the player up the tunnel, then leave. Only if the earlier scenes don't reproduce
  the dip (hypothesis 4).
- [ ] `snow_replay`: the playtest's seed (if Phase 18's check confirms it) and the report's coordinates (around
  500,4200), quiet and then with `snow_combo`'s events. Same world as the dip.
- [ ] One run of each, then 3 rotated runs of every scene that dips below ~55 fps. Record the table (fps per 300
  frames, work_ms and cpu_ms median/p95, entities/enemies) in the task notes. A scene "reproduces" if it sits
  near 48 fps for several windows with a steady frame time, as the report did.
- [ ] If nothing reproduces: stop building scenes, and wait for the next playtest occurrence with Phase 18's tools
  (live sample + work/cpu split). Record which hypotheses the sweep ruled out.

### Phase 20: find the cost

- [ ] The 48.0 question: in the reproducing scene (and heavy), look at the per-frame framelog. Is work_ms really
  ~20.8 ms every frame, or does the swap hold a fixed time? Compare uncapped (`UNCAPPED=1`): if uncapped runs well
  above 48, the cap was pacing, and the fix is in the swap path, not the simulation.
- [ ] Wine comparison of the reproducing scene, 3 rotated runs (as PLAN4 Phase 14). If Wine is as slow or slower,
  it's the game's own cost, and the target becomes ordinary optimization.
- [ ] Profile it with `tools/perfprof.sh` (`SCENE=<the scene>`): buckets per thread, top guest functions, and a
  joblog if main waits on the workers. Put it next to PROFILE.md's heavy and jungle profiles. Is it a known cost
  (the cell simulation, Box2D) at a new scale, or something the other scenes never hit?

### Phase 21: fix what the profile shows

- [ ] Decided by Phase 20. Each change gets its own interleaved A/B on the reproducing scene plus heavy and jungle
  (no regressions), and `tools/check.sh`.

### Phase 22: wrap up

- [ ] Final numbers for all scenes (flood, heavy, jungle, and the snow scene), native vs Wine, capped.
- [ ] Rebuild `build/Noita.app` with `--telemetry` and have the user play again, into the snow level and (still
  open from PLAN4) the jungle. Match any dips against the log and, if one happens live, `tools/playprof.sh`.
