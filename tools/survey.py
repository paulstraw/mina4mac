"""Try to lift every function of one module; report success rate and the top blockers.

  uv run tools/survey.py [module]   (writes build/<module>/survey.pkl)
"""
import argparse
import collections
import pickle
import re
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from lift import FnLifter, Program, Unsupported  # noqa: E402
from pe import MODULES, build_dir  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("module", nargs="?", default="noita", choices=MODULES)
    prog = Program(ap.parse_args().module)
    starts = sorted(a for a in prog.all_starts | prog.direct_calls if prog.t_lo <= a < prog.t_hi)
    ok, fail = {}, {}
    why = collections.Counter()
    t0 = time.time()
    for n, a in enumerate(starts):
        try:
            lf = FnLifter(prog, a)
            lf.lift()
            ok[a] = dict(callees=lf.callees, n=len(lf.body_set),
                         indirect=any("guest_call" in l for l in lf.lines),
                         imports=bool(lf.import_slots))
        except Unsupported as e:
            msg = str(e)
            fail[a] = msg
            key = re.sub(r"^0x[0-9a-f]+ ", "", msg).split(" ")[0]
            if "undecoded" in msg or "outside body" in msg or "falls off" in msg:
                key = re.sub(r"0x[0-9a-f]+", "", msg.split(": ", 1)[-1]).strip()
            why[key] += 1
        if n % 10000 == 0:
            print(f"  {n}/{len(starts)} {time.time() - t0:.0f}s", file=sys.stderr)
    pickle.dump(dict(ok=ok, fail=fail), open(build_dir(prog.module) / "survey.pkl", "wb"))
    insn_ok = sum(v["n"] for v in ok.values())
    print(f"functions lifted {len(ok):,}/{len(starts):,} ({len(ok) / len(starts):.1%}), {insn_ok:,} insns")
    print("top blockers (functions blocked):")
    for k, v in why.most_common(30):
        print(f"  {v:6}  {k}")


if __name__ == "__main__":
    main()
