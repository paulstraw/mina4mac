"""Recursive-descent code discovery over one module (default noita.exe).

Seeds: the entry point, exports in .text, every relocation whose target lies in .text (function
pointers, vtables, callbacks, jump-table entries), and every direct call target found along the way.
Output: build/<module>/discover.pkl (decoded instructions + function starts + jump tables), and a
coverage report.

  uv run tools/discover.py [module]
"""
import argparse
import collections
import pickle
import sys
import time
from pathlib import Path

import capstone
from capstone import x86

sys.path.insert(0, str(Path(__file__).parent))
from pe import MODULES, build_dir, load  # noqa: E402


# Mnemonics that compiled 32-bit user code never contains; seeing one means we are decoding data.
JUNK = {"in", "out", "insb", "insw", "insd", "outsb", "outsw", "outsd", "arpl", "bound", "les", "lds",
        "retf", "iretd", "hlt", "into", "int1", "salc", "aam", "aad", "daa", "das", "aaa", "aas",
        "lcall", "ljmp", "cli", "sti", "pushal", "popal", "popaw", "pushaw", "xlatb", "loope", "loopne",
        "sldt", "str", "lsl", "lar"}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("module", nargs="?", default="noita", choices=MODULES)
    args = ap.parse_args()
    img = load(args.module)
    t_lo, t_hi = img.section(".text")
    text = img.read(t_lo, t_hi - t_lo)

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True

    reloc_sites = img.relocs
    reloc_site_set = set(reloc_sites)
    # Relocation targets that land in .text.
    text_ptrs = {}
    for site in reloc_sites:
        v = img.u32(site)
        if t_lo <= v < t_hi:
            text_ptrs[site] = v

    ptr_targets = set(text_ptrs.values())
    insns = {}  # addr -> (size, mnemonic, op_str)
    call_targets = set()
    jump_tables = {}  # jmp addr -> [targets]
    unresolved_indirect_jmp = []
    import_calls = collections.Counter()

    # Some DLLs (msvcp120) merge .rdata into .text, so pointer seeds may point at data. There, weak
    # seeds (pointers, exports, relocated immediates) must look like code, and .text-resident
    # pointers (vtables in the merged rdata) are seeds too once their site is known not to be code.
    merged = any(t_lo <= img.base + d.VirtualAddress < t_hi
                 for d in img.pe.OPTIONAL_HEADER.DATA_DIRECTORY[:11] if d.Size)

    def plausible(addr, limit=64):
        """Straight-line decode from addr reaches ret/jmp without junk that compilers never emit."""
        if not merged:
            return True
        for _ in range(limit):
            if not t_lo <= addr < t_hi:
                return False
            off = addr - t_lo
            i = next(md.disasm(text[off:off + 16], addr, 1), None)
            if i is None or i.mnemonic in JUNK or i.bytes[:2] == b"\0\0" or \
                    i.mnemonic in ("push", "pop") and i.op_str in ("cs", "ds", "es", "ss"):
                return False
            if i.mnemonic in ("ret", "jmp") or addr in insns:
                return True
            addr += i.size
        return True

    seeds = [v for s, v in text_ptrs.items() if not (t_lo <= s < t_hi)]  # pointers from data sections
    seeds += [v for v in img.exports if t_lo <= v < t_hi]
    seeds = [v for v in seeds if plausible(v)]
    work = [img.entry] + seeds
    call_targets.add(img.entry)
    call_targets.update(seeds)
    seen_starts = set()

    def decode_from(addr):
        while t_lo <= addr < t_hi and addr not in insns:
            off = addr - t_lo
            try:
                i = next(md.disasm(text[off:off + 16], addr, 1))
            except StopIteration:
                return
            insns[addr] = (i.size, i.mnemonic, i.op_str)
            m = i.mnemonic
            nxt = addr + i.size
            if not (i.group(x86.X86_GRP_CALL) or i.group(x86.X86_GRP_JUMP)):
                for op in i.operands:
                    if op.type == x86.X86_OP_IMM:
                        v = op.imm & 0xFFFFFFFF
                        if t_lo <= v < t_hi and any(r in reloc_site_set for r in range(addr + 1, nxt)) \
                                and plausible(v):
                            call_targets.add(v)
                            work.append(v)
            if i.group(x86.X86_GRP_CALL) or i.group(x86.X86_GRP_JUMP):
                op = i.operands[0]
                if op.type == x86.X86_OP_IMM:
                    tgt = op.imm & 0xFFFFFFFF
                    if i.group(x86.X86_GRP_CALL):
                        call_targets.add(tgt)
                    work.append(tgt)
                elif op.type == x86.X86_OP_MEM:
                    if op.mem.base == 0 and op.mem.index == 0:
                        slot = op.mem.disp & 0xFFFFFFFF
                        if slot in img.imports:
                            import_calls[img.imports[slot]] += 1
                    elif m == "jmp" and op.mem.index != 0 and op.mem.scale == 4 and op.mem.base == 0:
                        tbl = op.mem.disp & 0xFFFFFFFF
                        tgts = []
                        p = tbl
                        while p in reloc_site_set:
                            v = img.u32(p)
                            if not (t_lo <= v < t_hi):
                                break
                            tgts.append(v)
                            p += 4
                            if p in ptr_targets:  # table runs into code
                                break
                        jump_tables[addr] = (tbl, tgts)
                        work.extend(tgts)
                    elif m == "jmp":
                        unresolved_indirect_jmp.append(addr)
                elif m == "jmp":
                    unresolved_indirect_jmp.append(addr)
                if m == "jmp":
                    return
            if m in ("ret", "retf", "iretd", "hlt", "ud2", "int3"):
                return
            addr = nxt

    def code_bytes():
        cov = set()
        for a, (sz, _, _) in insns.items():
            cov.update(range(a, a + sz))
        return cov

    t0 = time.time()
    while work:
        while work:
            a = work.pop()
            if a in seen_starts:
                continue
            seen_starts.add(a)
            decode_from(a)
        if merged:
            cov = code_bytes()
            new = {v for s, v in text_ptrs.items() if t_lo <= s < t_hi and s not in cov
                   and v not in seen_starts and v not in cov and plausible(v)}
            call_targets.update(new)
            work.extend(new)
    dt = time.time() - t0

    covered = sum(s for s, _, _ in insns.values())
    # Bytes not covered that are just int3/nop padding.
    cov = bytearray(t_hi - t_lo)
    for a, (s, _, _) in insns.items():
        cov[a - t_lo:a - t_lo + s] = b"\1" * s
    table_bytes = sum(4 * len(t) for _, t in jump_tables.values())
    uncovered = [i for i, c in enumerate(cov) if not c]
    pad = sum(1 for i in uncovered if text[i] in (0xCC, 0x90))
    in_text_reloc_data = sum(4 for s in text_ptrs if t_lo <= s < t_hi and not cov[s - t_lo])

    mn = collections.Counter(m for _, m, _ in insns.values())
    fs_use = sum(1 for _, m, o in insns.values() if "fs:" in o)
    x87 = sum(c for m, c in mn.items() if m.startswith("f") and m not in ("fs",))

    OUT = build_dir(args.module)
    OUT.mkdir(parents=True, exist_ok=True)
    with open(OUT / "discover.pkl", "wb") as f:
        pickle.dump(dict(insns=insns, calls=sorted(call_targets), jump_tables=jump_tables,
                         unresolved=unresolved_indirect_jmp, import_calls=import_calls), f)

    tsz = t_hi - t_lo
    print(f"decode time {dt:.1f}s")
    print(f".text size            {tsz:>10,}")
    print(f"decoded instructions  {len(insns):>10,}  ({covered:,} bytes, {covered / tsz:.1%})")
    print(f"uncovered padding     {pad:>10,}")
    print(f"jump-table data       {table_bytes:>10,}  ({len(jump_tables)} tables)")
    print(f"other reloc data in .text {in_text_reloc_data:>6,}")
    rest = tsz - covered - pad - table_bytes
    print(f"unexplained bytes     {rest:>10,}  ({rest / tsz:.2%})")
    print(f"function starts       {len(call_targets):>10,}")
    print(f"unresolved indirect jmp {len(unresolved_indirect_jmp):>8,}")
    print(f"fs: segment uses      {fs_use:>10,}")
    print(f"x87 instructions      {x87:>10,}")
    print(f"distinct mnemonics    {len(mn):>10,}")
    print("mnemonics:", " ".join(f"{m}:{c}" for m, c in mn.most_common()))


if __name__ == "__main__":
    main()
