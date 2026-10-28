"""Generate runtime/steam_gen.h from the Steamworks SDK headers the game was built with (1.45/1.46).

The output holds facts about the API, not Valve's code, so it is committed and the build doesn't need the SDK:
  - For each interface version the game requests (noita.exe's strings), its methods in MSVC vtable order (the
    guest's slots: MSVC groups overloads at the first one's position, in reverse order) with each method's
    declaration index (the host's slot: clang keeps declaration order). Overloads get _0, _1... suffixes in
    declaration order.
  - For each callback struct (k_iCallback), its fields' sizes and offsets in the Windows layout (pack 8) and
    the macOS one (pack 4), which runtime/steam.c converts between. CSteamID and CGameID are pack(1) classes, so
    they are 1-aligned in both.

  uv run tools/gen_steam.py <sdk>/public/steam
"""
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "runtime/steam_gen.h"

# Interface version string -> (header, class, short name for thunk names)
IFACES = {
    "SteamUser020": ("isteamuser", "ISteamUser"),
    "SteamFriends017": ("isteamfriends", "ISteamFriends"),
    "SteamUtils009": ("isteamutils", "ISteamUtils"),
    "STEAMREMOTESTORAGE_INTERFACE_VERSION014": ("isteamremotestorage", "ISteamRemoteStorage"),
    "STEAMUSERSTATS_INTERFACE_VERSION011": ("isteamuserstats", "ISteamUserStats"),
    "STEAMUGC_INTERFACE_VERSION013": ("isteamugc", "ISteamUGC"),
}

BASE = {  # scalar type -> (size, natural alignment); None: not convertible (host pointers are 8 bytes)
    "uint8": (1, 1), "int8": (1, 1), "char": (1, 1), "_Bool": (1, 1), "bool": (1, 1), "unsigned char": (1, 1),
    "uint16": (2, 2), "int16": (2, 2), "unsigned short": (2, 2), "short": (2, 2),
    "uint32": (4, 4), "int32": (4, 4), "int": (4, 4), "unsigned int": (4, 4), "float": (4, 4),
    "uint64": (8, 8), "int64": (8, 8), "double": (8, 8), "unsigned long long": (8, 8), "long long": (8, 8),
    "class CSteamID": (8, 1), "CSteamID": (8, 1), "class CGameID": (8, 1), "CGameID": (8, 1),
}


def strip(body):
    body = re.sub(r"//.*", "", body)
    return re.sub(r"/\*.*?\*/", "", body, flags=re.S)


def methods(hdr, cls):
    s = hdr.read_text(encoding="latin-1")
    i = re.search(r"class " + cls + r"\s*\{", s).start()
    body = strip(s[i:s.index("\n};", i)])
    body = re.sub(r"STEAM_\w+\s*(\([^()]*\))?", "", body)
    decls = [" ".join(m.split()) for m in re.findall(r"virtual\s+([^;]*?)\s*=\s*0\s*;", body, flags=re.S)]
    names = [re.search(r"(\w+)\s*\(", d).group(1) for d in decls]
    uniq = []
    for k, n in enumerate(names):
        same = [j for j, m in enumerate(names) if m == n]
        uniq.append(f"{n}_{same.index(k)}" if len(same) > 1 else n)
    order, seen = [], set()
    for n in names:
        if n not in seen:
            seen.add(n)
            order += [j for j, m in enumerate(names) if m == n][::-1]
    return [(uniq[d], d, decls[d]) for d in order]


def callback_ids(sdk):
    bases = {}
    for h in sdk.glob("*.h"):
        for name, v in re.findall(r"enum\s*\{\s*(k_i\w+Callbacks)\s*=\s*(\d+)\s*\}", h.read_text(encoding="latin-1")):
            bases[name] = int(v)
    ids = {}
    for h in sdk.glob("*.h"):
        s = strip(h.read_text(encoding="latin-1"))
        for name, base, n in re.findall(r"struct\s+(\w+)\s*\{\s*enum\s*\{\s*k_iCallback\s*=\s*(k_i\w+)\s*\+\s*(\d+)\s*\}", s):
            ids[name] = bases[base] + int(n)
    return ids


def header_fields(sdk, name):
    """Fields of a struct steam_api.json lacks, from its header: one "type name;" or "type name[N];" per line."""
    for h in sdk.glob("*.h"):
        s = strip(h.read_text(encoding="latin-1"))
        m = re.search(r"struct\s+" + name + r"\s*\{(.*?)\n\s*\};", s, flags=re.S)
        if m:
            body = re.sub(r"enum\s*\{[^}]*\}\s*;", "", m.group(1))
            out = []
            for t, n, arr in re.findall(r"^\s*([\w ]+?)\s+(\w+)\s*(\[\s*\w+\s*\])?\s*;", body, flags=re.M):
                if arr:
                    k = arr.strip("[] ")
                    if not k.isdigit():
                        return None
                    t = f"{t} [{k}]"
                out.append({"fieldname": n, "fieldtype": t})
            return out
    return None


def layout(fields, typedefs, maxalign):
    off = size = align = 0
    out = []
    for f in fields:
        t = f["fieldtype"].replace("const ", "").strip()
        n = 1
        m = re.fullmatch(r"(.+?)\s*\[(\d+)\]", t)
        if m:
            t, n = m.group(1).strip(), int(m.group(2))
        while t in typedefs:
            t = typedefs[t]
        if t.startswith("enum "):
            t = "int"
        if t not in BASE:
            return None
        sz, al = BASE[t]
        al = min(al, maxalign)
        off = (off + al - 1) // al * al
        out.append((sz * n, off))
        off += sz * n
        align = max(align, al)
    size = (off + align - 1) // align * align if align else 0
    return out, size


def main(sdk):
    sdk = Path(sdk)
    api = json.loads((sdk / "steam_api.json").read_text())
    typedefs = {t["typedef"]: t["type"] for t in api["typedefs"]}
    lines = ["// Generated by tools/gen_steam.py from the Steamworks SDK 1.46 headers. Do not edit.", "#pragma once", ""]
    lines.append("typedef struct { const char *name; int decl; } SteamMethod;")
    lines.append("typedef struct { const char *version, *cls; int n; const SteamMethod *m; } SteamIface;")
    for ver, (h, cls) in IFACES.items():
        ms = methods(sdk / f"{h}.h", cls)
        lines.append(f"static const SteamMethod STEAM_M_{cls}[] = {{")
        lines += [f'    {{"{n}", {d}}},' for n, d, _ in ms]  # names and indices only: no SDK text
        lines.append("};")
        for slot, (n, d, _) in enumerate(ms):
            lines.append(f"enum {{ {cls}_{n} = {d} }};")
    lines.append("static const SteamIface STEAM_IFACES[] = {")
    lines += [f'    {{"{v}", "{c}", sizeof STEAM_M_{c} / sizeof *STEAM_M_{c}, STEAM_M_{c}}},' for v, (_, c) in IFACES.items()]
    lines.append("};")
    lines.append("enum { STEAM_NIFACES = sizeof STEAM_IFACES / sizeof *STEAM_IFACES };")
    lines.append("")
    lines.append("// Callback structs: fields as {size, Windows offset, macOS offset}; n = -1: not convertible.")
    lines.append("typedef struct { int size, win, mac; } SteamField;")
    lines.append("typedef struct { int id; const char *name; int win_size, mac_size, n; SteamField f[24]; } SteamCallback;")
    lines.append("static const SteamCallback STEAM_CALLBACKS[] = {")
    ids = callback_ids(sdk)
    structs = {s["struct"]: s["fields"] for s in api["structs"]}
    bad = []
    for name, cid in sorted(ids.items(), key=lambda x: x[1]):
        fields = structs.get(name)
        if fields is None:
            fields = header_fields(sdk, name)
        if fields == []:  # an empty struct: C++ gives it one byte on both sides
            lines.append(f'    {{{cid}, "{name}", 1, 1, 0, {{{{0}}}}}},')
            continue
        w = fields is not None and layout(fields, typedefs, 8)
        m = fields is not None and layout(fields, typedefs, 4)
        if not w or not m or len(fields) > 24:
            bad.append(name)
            lines.append(f'    {{{cid}, "{name}", 0, 0, -1, {{{{0}}}}}},')
            continue
        fs = ", ".join(f"{{{s}, {wo}, {mo}}}" for (s, wo), (_, mo) in zip(w[0], m[0]))
        lines.append(f'    {{{cid}, "{name}", {w[1]}, {m[1]}, {len(fields)}, {{{fs}}}}},')
    lines.append("};")
    lines.append("enum { STEAM_NCALLBACKS = sizeof STEAM_CALLBACKS / sizeof *STEAM_CALLBACKS };")
    OUT.write_text("\n".join(lines) + "\n")
    print(f"{OUT.relative_to(ROOT)}: {len(IFACES)} interfaces, {len(ids)} callbacks ({len(bad)} not convertible: {', '.join(bad)})")


if __name__ == "__main__":
    main(sys.argv[1])
