"""What is guest function F_<addr>? Prints what helps name it: size, RTTI class of any vtable holding it, the strings
and imports it references, its direct callers and callees, and (with -d) its disassembly.

  uv run tools/fninfo.py 70a520 709960 [-d] [--module noita]
"""
import argparse
import bisect
import collections
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from lift import Program  # noqa: E402

HEX = re.compile(r"0x[0-9a-f]+")


class Info:
    def __init__(self, module):
        self.p = Program(module)
        self.img = self.p.img
        self.starts = sorted(a for a in self.p.all_starts | self.p.direct_calls if self.p.t_lo <= a < self.p.t_hi)
        self.callers = collections.defaultdict(set)
        for a, (_, m, o) in self.p.insns.items():
            if m in ("call", "jmp") and o.startswith("0x"):
                self.callers[int(o, 16)].add(a)
        self.rdata = [(lo, hi) for n, lo, hi, _ in self.img.sections if n in (".rdata", ".data")]
        self._vt = None

    def owner(self, a):  # the function start at or before a
        k = bisect.bisect_right(self.starts, a) - 1
        return self.starts[k] if k >= 0 else None

    def string(self, va):
        if not any(lo <= va < hi for lo, hi in self.rdata):
            return None
        b = self.img.read(va, 96)
        s = b.split(b"\0")[0]
        if len(s) >= 4 and all(32 <= c < 127 for c in s):
            return s.decode()
        w = b.decode("utf-16-le", "ignore").split("\0")[0]
        return f'L"{w}"' if len(w) >= 4 and w.isprintable() and w.isascii() else None

    def vtables(self):
        """function VA -> [(class name, slot)], from MSVC RTTI (vtable[-1] is a CompleteObjectLocator)."""
        if self._vt is not None:
            return self._vt
        self._vt = collections.defaultdict(list)
        lo, hi = self.img.section(".rdata")
        tlo, thi = self.p.t_lo, self.p.t_hi
        for va in range(lo + 4, hi - 4, 4):
            col = self.img.u32(va - 4)
            if not (lo <= col < hi - 20) or self.img.u32(col) != 0:
                continue
            td = self.img.u32(col + 12)
            name = self.string(td + 8) if lo <= td < self.img.section(".data")[1] else None
            if not name or not name.startswith(".?A"):
                continue
            k = 0
            while va + 4 * k < hi and tlo <= self.img.u32(va + 4 * k) < thi:
                self._vt[self.img.u32(va + 4 * k)].append((name[4:].split("@@")[0], k))
                k += 1
        return self._vt

    def show(self, a, dis):
        body = sorted(self.p.function_body(a))
        print(f"== F_{a:08x}: {len(body)} instructions")
        vt = self.vtables().get(a)
        if vt:
            print("  vtable slots:", ", ".join(f"{c}[{k}]" for c, k in vt[:6]))
        strs, imps, callees = [], [], []
        for i in body:
            _, m, o = self.p.insns[i]
            if m == "call" and o.startswith("0x"):
                callees.append(int(o, 16))
            for h in HEX.findall(o):
                v = int(h, 16)
                if v in self.img.imports:
                    imps.append(self.img.imports[v][1])
                elif (s := self.string(v)) and s not in strs:
                    strs.append(s)
        for s in strs[:12]:
            print(f"  string: {s!r}")
        if imps:
            print("  imports:", ", ".join(sorted(set(imps))))
        cs = collections.Counter(callees)
        if cs:
            print("  calls:", ", ".join(f"F_{c:08x}" + (f"x{n}" if n > 1 else "") for c, n in cs.most_common(12)))
        cr = collections.Counter(self.owner(s) for s in self.callers.get(a, ()))
        if cr:
            print("  called by:", ", ".join(f"F_{c:08x}" for c, _ in cr.most_common(10)))
        if dis:
            for i in body:
                _, m, o = self.p.insns[i]
                print(f"    {i:08x}  {m} {o}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("addrs", nargs="+")
    ap.add_argument("-d", action="store_true", help="disassemble")
    ap.add_argument("--module", default="noita")
    a = ap.parse_args()
    info = Info(a.module)
    for s in a.addrs:
        info.show(int(s.removeprefix("F_"), 16), a.d)


if __name__ == "__main__":
    main()
