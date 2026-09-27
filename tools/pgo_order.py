"""Write a linker order file from a merged PGO profile: every function that ran, hottest first (by the sum of its
block counts, a proxy for time spent), so ld64 packs the hot code together. Static functions (profile names like
"/path/rt.c;call_thunk") are written as "rt.o:_call_thunk".

  uv run tools/pgo_order.py <profdata> <out.order>
"""
import re
import subprocess
import sys
from pathlib import Path


def main():
    prof, out = sys.argv[1:3]
    profdata = subprocess.run(["clang", "-print-prog-name=llvm-profdata"], capture_output=True, text=True,
                              check=True).stdout.strip()
    text = subprocess.run([profdata, "show", "--all-functions", "--counts", prof], capture_output=True, text=True,
                          check=True).stdout
    rows, name = [], None
    for line in text.splitlines():
        if m := re.match(r"  (\S.*):$", line):
            name = m.group(1)
        elif (m := re.match(r"\s+Block counts: \[(.*)\]", line)) and name:
            total = sum(int(x) for x in m.group(1).split(",") if x.strip())
            if total:
                rows.append((total, name))
    rows.sort(reverse=True)
    syms = []
    for _, n in rows:
        if ";" in n:
            path, fn = n.rsplit(";", 1)
            syms.append(f"{Path(path).stem}.o:_{fn}")
        else:
            syms.append(f"_{n}")
    Path(out).write_text("\n".join(syms) + "\n")
    print(f"{out}: {len(syms):,} functions")


if __name__ == "__main__":
    main()
