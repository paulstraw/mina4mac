"""Static inventory of how noita.exe uses its 171 lua51 imports (input for the LuaJIT bridge).

For every lua51 IAT slot it finds each reference and classifies it:
  - called: `call/jmp [slot]`, or the slot value loaded into a register that is later `call reg`ed (MSVC caches
    import pointers in registers);
  - sandbox: the value is stored into the address list of the mod sandbox's patch function (SANDBOX_FN), which
    overwrites each listed function with PATCH (`mov dword ptr [0], 0`) and can restore the originals.
It also reports the lua_pushcclosure upvalue counts, the pseudo-indices used as immediates, the Lua library table
the state constructor opens, and the lua_Debug users.

  uv run tools/luasurvey.py
"""
import bisect
import re
import struct
import sys
from collections import Counter, defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from lift import Program  # noqa: E402

SANDBOX_FN = (0x7ee720, 0x7eee5c)  # void sandbox(bool patch) (cl): the function's address range
PATCH = bytes.fromhex("c70500000000" "00000000")  # stored at [ebp-0x210] by SANDBOX_FN
STATE_CTOR = 0x7ed880  # LuaState::init: luaL_newstate, then luaL_openlibs or the LIBS table
LIBS = 0xffa080  # {const char *name, lua_CFunction open} pairs, NULL-terminated
REMOVED_GLOBALS = 0x11a0aa0  # NULL-terminated list of global names set to nil
PSEUDO = {0xffffd8f0: "LUA_REGISTRYINDEX", 0xffffd8ef: "LUA_ENVIRONINDEX", 0xffffd8ee: "LUA_GLOBALSINDEX"}


def main():
    p = Program("noita")
    img = p.img
    rd = lambda a: struct.unpack("<I", img.read(a, 4))[0]  # noqa: E731
    cstr = lambda a: img.read(a, 256).split(b"\0")[0].decode("latin1")  # noqa: E731
    slots = {s: n for s, (d, n) in img.imports.items() if d.lower() == "lua51.dll"}
    addrs = sorted(p.insns)

    def pushes(site):  # operands of the pushes before a call, last push (first argument) first
        i, out = bisect.bisect_left(addrs, site), []
        for b in reversed(addrs[max(0, i - 20):i]):
            _, m, o = p.insns[b]
            if m == "push":
                out.append(o)
            if m == "call":
                break
        return out

    sites, sandbox = defaultdict(list), set()
    for a, (_, m, o) in p.insns.items():
        for h in re.findall(r"0x[0-9a-f]+", o):
            n = slots.get(int(h, 16))
            if n is None:
                continue
            if m in ("call", "jmp"):
                sites[n].append(a)
            elif SANDBOX_FN[0] <= a < SANDBOX_FN[1]:
                sandbox.add(n)
            else:  # mov reg, [slot]: follow reg to `call reg` until it's redefined
                reg = o.split(",")[0]
                for b in addrs[bisect.bisect_right(addrs, a):][:400]:
                    _, m2, o2 = p.insns[b]
                    if m2 == "call" and o2 == reg:
                        sites[n].append(b)
                    ops = [x.strip() for x in o2.split(",")]
                    if (ops[0] == reg and m2 in ("mov", "lea", "pop", "movzx", "xor")) or m2.startswith("ret") \
                            or (m2 == "call" and reg in ("eax", "ecx", "edx")):
                        break
    code = img.read(SANDBOX_FN[0], SANDBOX_FN[1] - SANDBOX_FN[0])
    assert b"\xc7\x85\xf0\xfd\xff\xff\xc7\x05\x00\x00" in code, "patch bytes not found in the sandbox function"

    called = sorted(sites, key=lambda n: -len(sites[n]))
    print(f"lua51 imports: {len(slots)}; called: {len(called)}; only in the sandbox list: "
          f"{len(sandbox - set(sites))}; unreferenced: {len(set(slots.values()) - set(sites) - sandbox)}")
    print("called (call sites):", ", ".join(f"{n} {len(sites[n])}" for n in called))
    print(f"sandbox list ({len(sandbox)}, patched with {PATCH.hex()} = mov dword ptr [0], 0):",
          ", ".join(sorted(sandbox)))
    print("both called and in the sandbox list:", ", ".join(sorted(sandbox & set(sites))))
    ups = Counter(ps[2] for s in sites["lua_pushcclosure"] if len(ps := pushes(s)) > 2)
    print("lua_pushcclosure upvalue counts:", dict(ups))
    imm = Counter(int(h, 16) for _, _, o in p.insns.values() for h in re.findall(r"0xffffd8[0-9a-f]{2}", o))
    print("pseudo-index immediates:", {PSEUDO.get(k, hex(k)): v for k, v in imm.items()})
    libs = []
    for i in range(16):
        name, fn = rd(LIBS + 8 * i), rd(LIBS + 8 * i + 4)
        if not name:
            break
        _, m, o = p.insns[fn]  # an import stub: jmp [slot]
        libs.append(f"{cstr(name)}={slots[int(o.split('[')[1][:-1], 16)]}")
    print("state constructor opens (lua_pushcclosure(thunk) + lua_call):", ", ".join(libs))
    removed, i = [], 0
    while rd(REMOVED_GLOBALS + 4 * i):
        removed.append(cstr(rd(REMOVED_GLOBALS + 4 * i)))
        i += 1
    print("globals set to nil:", ", ".join(removed))
    starts = sorted(p.all_starts | p.direct_calls)
    fn_of = lambda a: starts[bisect.bisect_right(starts, a) - 1]  # noqa: E731
    for n in ("luaL_newstate", "lua_close", "luaL_openlibs", "lua_getstack", "lua_getinfo", "lua_tonumber",
              "lua_pushnumber", "lua_pushfstring", "lua_topointer", "lua_pushlightuserdata"):
        fns = sorted({fn_of(s) for s in sites.get(n, [])})
        print(f"{n}: {len(sites.get(n, []))} sites in {len(fns)} functions", [hex(f) for f in fns[:6]])


if __name__ == "__main__":
    main()
