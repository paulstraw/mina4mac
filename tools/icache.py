"""Pick inline-cache sites from indirect-call profiles (PLAN3 Phase 11).

A --icprof build (tools/build_all.py --icprof) run with MINA4MAC_ICPROF=<file> writes "site<TAB>target<TAB>calls"
lines. This merges one or more of them (each scaled to the same total, so scenes weigh equally), takes the hottest
sites until they cover --coverage of all calls, and for each keeps up to --max-targets targets that get at least
--min-share of the site's calls. The result (tools/icache_sites.txt by default) is what build_all.py --ic reads:
"site target [target...]" lines in hex, hottest site first. It holds only addresses in the exe.

  uv run tools/icache.py build/icprof/heavy.tsv build/icprof/flood.tsv [-o tools/icache_sites.txt]
"""
import argparse
import collections
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("profiles", nargs="+")
    ap.add_argument("-o", "--out", default=str(ROOT / "tools/icache_sites.txt"))
    ap.add_argument("--coverage", type=float, default=0.99, help="fraction of all indirect calls the sites cover")
    ap.add_argument("--min-share", type=float, default=0.05, help="a target's minimum share of its site's calls")
    ap.add_argument("--max-targets", type=int, default=2)
    args = ap.parse_args()

    sites = collections.defaultdict(collections.Counter)
    for p in args.profiles:
        rows = [line.split() for line in Path(p).read_text().splitlines() if line.strip()]
        total = sum(int(n) for *_, n in rows)
        for s, t, n in rows:
            sites[int(s, 16)][int(t, 16)] += int(n) / total
    total = sum(sum(c.values()) for c in sites.values())
    ranked = sorted(sites.items(), key=lambda kv: -sum(kv[1].values()))

    out, covered, cached = [], 0.0, 0.0
    for s, c in ranked:
        if covered >= args.coverage * total:
            break
        n = sum(c.values())
        covered += n
        ts = [(t, k) for t, k in c.most_common(args.max_targets) if k >= args.min_share * n]
        cached += sum(k for _, k in ts)
        if ts:
            out.append(f"{s:#x} " + " ".join(f"{t:#x}" for t, _ in ts) + f"  # {n / total:.2%}: "
                       + " ".join(f"{k / n:.0%}" for _, k in ts))
    Path(args.out).write_text(
        f"# tools/icache.py {' '.join(Path(p).name for p in args.profiles)}: {len(out)} sites, "
        f"{cached / total:.1%} of indirect calls hit a cached target\n" + "\n".join(out) + "\n")
    print(f"{len(out)} sites of {len(sites)}; they take {covered / total:.1%} of calls, "
          f"{cached / total:.1%} hit a cached target -> {args.out}")


if __name__ == "__main__":
    main()
