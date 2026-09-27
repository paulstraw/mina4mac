"""Summarize a MINA4MAC_JOBLOG file (runtime/joblog.c): how the game's job system spends a frame.

  uv run tools/joblog.py build/joblog/flood.txt [--framelog build/game/perfbench_frames.txt]

With --framelog (perfbench's MINA4MAC_FRAMELOG, which has the mod's start/end marks) only the benchmark's
measured frames count; otherwise every frame after the first 10 s. Prints, per frame: the main thread's time in
job barriers (by wait site), the workers' busy time, the jobs (count and duration by type), and per barrier how
long the slowest job ran, how busy the workers were while main waited, and how late main noticed the last job
ending. Then wake-up latency (condvar notify to worker running) and which cores the jobs ran on (0-1 are the
M1 Max's E-cores).
"""
import argparse
import bisect
import collections
import statistics


def pct(v, p):
    v = sorted(v)
    return v[min(len(v) - 1, int(len(v) * p))] if v else 0.0


def med(v):
    return statistics.median(v) if v else 0.0


def load(path):
    ev = collections.defaultdict(list)
    for line in open(path):
        f = line.split()
        if not f:
            continue
        k = f[0]
        if k == "j":  # slot fn inner t0 t1 cpu core0 core1
            ev[k].append((int(f[1]), int(f[2], 16), int(f[3], 16), float(f[4]), float(f[5]), float(f[6]), int(f[7]), int(f[8])))
        elif k == "w":  # slot site t0 t1 polls sleep
            ev[k].append((int(f[1]), int(f[2], 16), float(f[3]), float(f[4]), int(f[5]), float(f[6])))
        elif k == "c":
            ev[k].append((int(f[1]), float(f[2]), float(f[3])))
        elif k == "n":
            ev[k].append((int(f[1]), float(f[2]), int(f[3])))
        elif k == "f":
            ev[k].append((float(f[1]), float(f[2])))
        elif k == "t":
            ev[k].append((int(f[1]), int(f[2], 16)))
        elif k == "cpus":
            ev[k] = int(f[1])
    for k in ("j", "w", "c", "n", "f"):
        ev[k].sort(key=lambda e: e[1] if k == "f" else e[2] if k in ("j", "w") else e[1])
    ev["f"].sort()
    return ev


def window(ev, framelog):
    """(t_lo, t_hi, frames): from the end of one swap to the end of another."""
    swaps = [t1 for _, t1 in ev["f"]]
    if framelog:
        n = start = end = 0
        for line in open(framelog):
            if line.startswith("f "):
                n += 1
            elif line.startswith("mark start"):
                start = n
            elif line.startswith("mark end"):
                end = n
        # framelog line m (0-based) is written at joblog swap m + 1 (the first swap has no previous one)
        lo, hi = start + 1, (end or n) + 1
    else:
        lo = bisect.bisect_left(swaps, swaps[0] + 10e6) if swaps else 0
        hi = len(swaps) - 1
    lo, hi = max(0, lo), min(len(swaps) - 1, hi)
    return swaps[lo], swaps[hi], hi - lo


def overlap(a0, a1, b0, b1):
    return max(0.0, min(a1, b1) - max(a0, b0))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--framelog")
    a = ap.parse_args()
    ev = load(a.log)
    lo, hi, nf = window(ev, a.framelog)
    wall = hi - lo
    if nf <= 0:
        raise SystemExit("no frames in the window")
    jobs = [j for j in ev["j"] if lo <= j[3] and j[4] <= hi]
    waits = [w for w in ev["w"] if w[0] == 0 and lo <= w[2] and w[3] <= hi]
    threads = ev["t"]
    print(f"cpus reported {ev['cpus']}; guest threads created {len(threads)} "
          f"(entry points: {', '.join(f'{s:#x} x{n}' for s, n in collections.Counter(s for _, s in threads).items())})")
    frame_ms = wall / nf / 1e3
    swap = [t1 - t0 for t0, t1 in ev["f"] if lo < t1 <= hi]
    print(f"window: {nf} frames, {wall / 1e6:.1f} s, {frame_ms:.2f} ms/frame (swap {sum(swap) / nf / 1e3:.2f} ms)")

    print("\n== main thread: job barriers (wait loops polling the ConcRT scheduler id), per frame ==")
    by_site = collections.defaultdict(list)
    for w in waits:
        by_site[w[1]].append(w)
    tot = sum(w[3] - w[2] for w in waits)
    print(f"total {tot / nf / 1e3:.2f} ms/frame = {100 * tot / wall:.0f}% of main's wall time; {len(waits) / nf:.1f} waits/frame; "
          f"Sleep(0) {sum(w[5] for w in waits) / nf / 1e3:.2f} ms/frame, {sum(w[4] for w in waits) / nf:.0f} polls/frame")

    # Jobs of a barrier: those ending after the previous barrier on main ended, up to this one's end.
    ends = sorted((j[4], j) for j in jobs)
    end_t = [e for e, _ in ends]
    per = collections.defaultdict(float)
    for j in jobs:
        per[j[0]] += j[4] - j[3]
    pool = {s for s, t in per.items() if t > 0.01 * wall}  # the threads that do real work (the rest idle)
    nworkers = len(pool) or 1
    rows = []
    prev = lo
    for w in waits:
        i0, i1 = bisect.bisect_right(end_t, prev), bisect.bisect_right(end_t, w[3] + 50)
        js = [j for _, j in ends[i0:i1]]
        prev = w[3]
        if not js:
            continue
        last = max(js, key=lambda j: j[4])
        big = max(js, key=lambda j: j[4] - j[3])
        busy = sum(overlap(j[3], j[4], w[2], w[3]) for j in js if j[0] in pool)
        rows.append((w[1], w[3] - w[2], len(js), big[4] - big[3], sum(j[4] - j[3] for j in js),
                     max(0.0, w[3] - last[4]), busy / (nworkers * max(1e-9, w[3] - w[2])),
                     last[6] < 2 or last[7] < 2, max(0.0, big[4] - max(big[3], w[2]))))
    print(f"{'site':>10} {'n/frame':>7} {'wait ms':>8} {'% main':>6} {'jobs':>5} {'longest µs':>10} {'sum µs':>8} {'late µs':>7} {'util':>5} {'wait µs':>7} {'tail':>5} {'E last':>6}")
    agg = collections.defaultdict(list)
    for r in rows:
        agg[r[0]].append(r)
    for site, ws in sorted(by_site.items(), key=lambda kv: -sum(w[3] - w[2] for w in kv[1])):
        t = sum(w[3] - w[2] for w in ws)
        rs = agg.get(site, [])
        print(f"{site:#10x} {len(ws) / nf:7.2f} {t / nf / 1e3:8.3f} {100 * t / wall:5.1f}% {med([r[2] for r in rs]):5.0f} "
              f"{med([r[3] for r in rs]):10.0f} {med([r[4] for r in rs]):8.0f} {med([r[5] for r in rs]):7.0f} "
              f"{100 * med([r[6] for r in rs]):4.0f}% {med([r[1] for r in rs]):7.0f} {med([r[8] for r in rs]):5.0f} "
              f"{100 * sum(r[7] for r in rs) / max(1, len(rs)):5.0f}%")
    print("  (medians per barrier: jobs, longest job, sum of job time, main's delay after the last job ended, how busy\n"
          f"   the {nworkers} working job threads were during the wait, the wait itself, how much of the longest job was\n"
          "   still left when main started waiting; E last = share of barriers whose last job ended on an E-core)")

    print("\n== workers ==")
    busy = sum(per.values())
    print(f"{len(per)} threads ran jobs: busy {busy / wall:.2f} cores, "
          + ", ".join(f"{s}:{100 * t / wall:.0f}%" for s, t in sorted(per.items())))
    cpu = sum(j[5] for j in jobs)
    idle = [s for s in per if s not in pool]
    if idle:
        print(f"  threads {min(idle)}-{max(idle)} are a second pool that ran {sum(1 for j in jobs if j[0] not in pool) / nf:.1f} jobs/frame")
    print(f"{len(jobs) / nf:.0f} jobs/frame; job time {busy / nf / 1e3:.2f} ms/frame; thread CPU / wall inside jobs {cpu / busy:.2f}")
    kinds = collections.defaultdict(list)
    for j in jobs:
        kinds[j[2] or j[1]].append(j[4] - j[3])
    print(f"{'job type':>10} {'n/frame':>7} {'ms/frame':>8} {'median µs':>9} {'p95 µs':>7} {'max µs':>7}")
    for k, d in sorted(kinds.items(), key=lambda kv: -sum(kv[1]))[:12]:
        print(f"{k:#10x} {len(d) / nf:7.1f} {sum(d) / nf / 1e3:8.3f} {med(d):9.0f} {pct(d, 0.95):7.0f} {max(d):7.0f}")

    print("\n== wake-up latency (a notify on any thread -> a worker's condvar wait returns) ==")
    notes = sorted(n[1] for n in ev["n"] if lo <= n[1] <= hi)
    lat = []
    for s, t0, t1 in ev["c"]:
        if s > 0 and lo <= t1 <= hi:
            i = bisect.bisect_right(notes, t1) - 1
            if i >= 0 and notes[i] >= t0:
                lat.append(t1 - notes[i])
    print(f"{len(lat) / nf:.0f} wakes/frame: median {med(lat):.0f} µs, p95 {pct(lat, 0.95):.0f} µs, p99 {pct(lat, 0.99):.0f} µs; "
          f"notifies {len(notes) / nf:.0f}/frame")
    gap = []  # a worker's time from one job's end to its next job's start within a barrier: queue/wake overhead
    by_w = collections.defaultdict(list)
    for j in jobs:
        by_w[j[0]].append(j)
    for js in by_w.values():
        js.sort(key=lambda j: j[3])
        gap += [b[3] - a[4] for a, b in zip(js, js[1:]) if b[3] - a[4] < 2000]
    print(f"worker gap between back-to-back jobs (< 2 ms): median {med(gap):.1f} µs, p95 {pct(gap, 0.95):.1f} µs")

    print("\n== cores (pthread_cpu_number_np at job start; 0-1 are E-cores) ==")
    core_t = collections.defaultdict(float)
    for j in jobs:
        core_t[j[6]] += j[4] - j[3]
    print("job time by core: " + ", ".join(f"{c}:{100 * t / busy:.1f}%" for c, t in sorted(core_t.items())))
    e = [j for j in jobs if j[6] < 2]
    migr = sum(1 for j in jobs if (j[6] < 2) != (j[7] < 2))
    print(f"jobs started on an E-core: {100 * len(e) / max(1, len(jobs)):.1f}% ({100 * sum(j[4] - j[3] for j in e) / busy:.1f}% of job time); "
          f"moved between E and P during a job: {100 * migr / max(1, len(jobs)):.1f}%")
    for k, _ in sorted(kinds.items(), key=lambda kv: -sum(kv[1]))[:3]:
        pe = [j[4] - j[3] for j in jobs if (j[2] or j[1]) == k and j[6] >= 2 and j[7] >= 2]
        ee = [j[4] - j[3] for j in jobs if (j[2] or j[1]) == k and j[6] < 2 and j[7] < 2]
        if pe and ee:
            print(f"  {k:#x}: median {med(pe):.0f} µs on P, {med(ee):.0f} µs on E ({len(ee)} jobs)")
    # barrier tails: was the longest job of a barrier on an E-core?
    tails = 0
    prev, n = lo, 0
    for w in waits:
        i0, i1 = bisect.bisect_right(end_t, prev), bisect.bisect_right(end_t, w[3] + 50)
        prev = w[3]
        if i1 > i0:
            n += 1
            tails += ends[i1 - 1][1][6] < 2 or ends[i1 - 1][1][7] < 2
    print(f"barriers whose last-finishing job ran on an E-core: {100 * tails / max(1, n):.1f}%")


if __name__ == "__main__":
    main()
