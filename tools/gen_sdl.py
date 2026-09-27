"""Generate the SDL2 bridge: host thunks that implement noita.exe's SDL2 imports by calling the host's
(Homebrew) SDL2, marshalling arguments and results between the 32-bit guest and the 64-bit host.

The declarations come from the installed SDL2 headers (preprocessed as the guest sees them), and the
struct layouts from clang's record-layout dumps of the same headers for both the guest (i686 MSVC) and
the host. Types are marshalled by what they are:
  - scalars and enums: read from the guest stack (64-bit and double take two slots);
  - pointers to scalars, and to structs whose layout is the same on both sides: guest memory directly;
  - pointers to structs whose layout differs (SDL_DisplayMode): copied field by field (generated g2h_/h2g_);
  - opaque objects (SDL_Window, SDL_Joystick, SDL_RWops, SDL_GLContext, ...): 32-bit guest handles;
  - SDL_Surface: a guest mirror struct, with pixels kept in guest memory (runtime/sdl2.c);
  - strings: guest strings are passed directly (paths are translated); returned strings are copied into
    a per-function guest buffer, valid until the next call, as SDL's own are.
SDL_PollEvent, SDL_FreeSurface and SDL_GL_SwapWindow are hand-written in runtime/sdl2.c, and the C-library-style helpers
SDL2main uses are HLE in runtime/sdl2_stdlib.c.

Writes build/gen_all/sdl2_gen.c (thunks) and build/gen_all/sdl2_layout.h (guest offsets: G_<struct>_<field>
and G_<struct>_SIZE). Exits non-zero if an import can't be marshalled.

  uv run tools/gen_sdl.py
"""
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from pe import ROOT, load  # noqa: E402

GEN = ROOT / "build/gen_all"
HLE = {"SDL_malloc", "SDL_free", "SDL_isspace", "SDL_wcslen", "SDL_iconv_string", "SDL_SetMainReady"}  # sdl2_stdlib.c
HAND = {"SDL_PollEvent", "SDL_FreeSurface", "SDL_GL_SwapWindow"}  # sdl2.c
EXTRA = {"SDL_GetVersion", "SDL_GetTicks"}  # not imported by the game; bridged for runtime/sdl_test.c
OPAQUE = {"SDL_RWops"}  # complete in the headers, but the game only passes it back to SDL
HANDLE_TYPEDEFS = {"SDL_GLContext"}  # typedef void *
PATHS = {("SDL_RWFromFile", "file")}  # Windows paths to translate
# SDL_Event members whose layout differs; runtime/sdl2.c converts these by hand.
EVENT_HAND = {"SDL_DropEvent", "SDL_UserEvent", "SDL_SysWMEvent", "SDL_TextEditingExtEvent"}
WIDE = {"Uint64": "ARG_I64", "Sint64": "ARG_I64", "double": "ARG_F64"}
ARGF = {"float": "ARG_F32"}

PRELUDE = "#define SDL_MAIN_HANDLED\n#include <SDL.h>\n"
# The guest's view: i686 MSVC layouts, with the platform macros SDL would use for Windows headers
# (sal.h, process.h, ...) removed, and SDL_config.h (Homebrew's, for macOS) replaced by the basics.
GUEST_CONFIG = ('#define SDL_config_h_\n#include "SDL_platform.h"\n'
                "#define HAVE_STDINT_H 1\n#define HAVE_STDARG_H 1\n#define HAVE_STDDEF_H 1\n")
GUEST = ["-target", "i686-pc-windows-msvc", "-ffreestanding", "-nostdlibinc", "-U_WIN32", "-U_MSC_VER"]


def sdl_include() -> str:
    flags = subprocess.run(["sdl2-config", "--cflags"], capture_output=True, text=True, check=True).stdout.split()
    return next(f[2:] for f in flags if f.startswith("-I"))


def clang(args, src, guest):
    work = ROOT / "build/sdl"
    work.mkdir(parents=True, exist_ok=True)
    (work / "probe.c").write_text(src)
    (work / "guest_config.h").write_text(GUEST_CONFIG)
    extra = [*GUEST, "-include", str(work / "guest_config.h")] if guest else []
    p = subprocess.run(["clang", *extra, "-I", sdl_include(), *args, str(work / "probe.c")],
                       capture_output=True, text=True)
    if p.returncode:
        sys.exit(f"clang failed:\n{p.stderr[:2000]}")
    return p.stdout


def layouts(guest):
    """Record name -> (size, [(offset, type, field)]) for the top-level fields of every SDL record."""
    out = clang(["-fsyntax-only", "-Xclang", "-fdump-record-layouts", "-Xclang", "-fdump-record-layouts-complete"],
                PRELUDE, guest)
    recs, cur = {}, None
    for line in out.splitlines():
        if m := re.match(r"\s*0 \| (?:struct|union) (SDL_\w+)$", line):
            cur = recs[m[1]] = [0, []]
        elif cur is None:
            continue
        elif m := re.match(r"\s*(\d+)(:\S+)? \|   (?! )(.*) (\w+)$", line):
            cur[1].append((int(m[1]), m[3].strip() + (" :bits" if m[2] else ""), m[4]))
        elif m := re.match(r"\s*\| \[sizeof=(\d+)", line):
            cur[0] = int(m[1])
            cur = None
    return {k: (v[0], v[1]) for k, v in recs.items()}


def declarations():
    """Function name -> (return type, [(type, name)]) as the guest's headers declare them."""
    text = " ".join(clang(["-E", "-P"], PRELUDE, True).split())
    out = {}
    for m in re.finditer(r"extern ([^;{}()]*?)\b(SDL_\w+) ?\(([^;{}]*?)\) ?;", text):
        params = []
        for k, p in enumerate(x.strip() for x in m[3].split(",")):
            if p in ("void", ""):
                continue
            if "(" in p or "[" in p or "..." in p:
                params.append((p, f"a{k}"))  # rejected by marshalling
                continue
            pm = re.match(r"(.*?[\s*])(\w+)$", p)
            if not pm or pm[1].strip() in ("", "const", "unsigned", "signed"):  # unnamed parameter
                pm = (None, p, f"a{k}")
            params.append((" ".join(pm[1].replace("*", " * ").split()), pm[2]))
        out[m[2]] = (" ".join(m[1].replace("*", " * ").split()), params)
    return out


def base_of(t):
    """Pointee type of a pointer type (const dropped), or None if not a single pointer."""
    parts = t.replace("const", "").split()
    if parts.count("*") != 1 or parts[-1] != "*":
        return None
    return " ".join(parts[:-1])


class Gen:
    def __init__(self):
        self.g = layouts(True)
        self.h = layouts(False)
        self.decls = declarations()
        self.converters = set()

    def same(self, name):
        return self.g[name] == self.h[name]

    def arg(self, fn, t, name, slot, pre, post):
        """C expression for parameter `name` of type t at stack slot `slot`; returns (expr, slots used)."""
        base = base_of(t)
        a = f"ARG({slot})"
        if t in WIDE:
            return f"({t}){WIDE[t]}({slot})", 2
        if t in ARGF:
            return f"{ARGF[t]}({slot})", 1
        if t in HANDLE_TYPEDEFS:
            return f"({t})sdl_host({a})", 1
        if base is None:
            if t in self.g or t.startswith("struct "):
                raise ValueError(f"struct by value: {t}")
            if "*" in t or "[" in t or "(" in t:
                raise ValueError(f"unsupported type: {t}")
            return f"({t}){a}", 1
        if base == "char":
            if (fn, name) in PATHS:
                pre.append(f"char p{slot}[4096];")
                pre.append(f'if ({a} && !host_path(ARG_STR({slot}), p{slot}, sizeof p{slot})) return ret_i32(c, 0);')
                return f"({a} ? p{slot} : NULL)", 1
            return f"ARG_STR({slot})", 1
        if base == "SDL_Surface":
            post.append(f"sdl_surface_sync(s{slot});")
            pre.append(f"SDL_Surface *s{slot} = sdl_surface_host({a});")
            return f"s{slot}", 1
        if base in OPAQUE or (base.startswith("SDL_") and base not in self.g):
            return f"({base} *)sdl_host({a})", 1
        if base in self.g and not self.same(base):
            if base == "SDL_Event":
                raise ValueError("SDL_Event (hand-written)")
            self.converters.add(base)
            pre.append(f"{base} v{slot};")
            pre.append(f"if ({a}) g2h_{base}({a}, &v{slot});")
            if "const" not in t:
                post.append(f"if ({a}) h2g_{base}(&v{slot}, {a});")
            return f"({a} ? &v{slot} : NULL)", 1
        return f"ARG_PTR({slot})", 1  # scalars, void, identical structs: guest memory as is

    def ret(self, fn, t):
        """Statement returning `r` of type t to the guest."""
        base = base_of(t)
        if t == "void":
            return None
        if t in ("float", "double"):
            return f"ret_f64(c, r);"
        if t in WIDE:
            return "ret_i64(c, (uint64_t)r);"
        if t in HANDLE_TYPEDEFS:
            return "ret_i32(c, sdl_handle(r));"
        if base is None:
            if t in self.g or "*" in t:
                raise ValueError(f"unsupported return: {t}")
            return "ret_i32(c, (uint32_t)(int32_t)r);"
        if base == "char":
            return f"static uint32_t buf; ret_i32(c, sdl_guest_str(&buf, r));"
        if base == "SDL_Surface":
            return "ret_i32(c, sdl_surface_guest(r));"
        if base in OPAQUE or (base.startswith("SDL_") and base not in self.g):
            return "ret_i32(c, sdl_handle(r));"
        raise ValueError(f"unsupported return: {t}")

    def thunk(self, fn):
        rt, params = self.decls[fn]
        pre, post, args, slot = [], [], [], 0
        for t, name in params:
            e, n = self.arg(fn, t, name, slot, pre, post)
            args.append(e)
            slot += n
        call = f"{fn}({', '.join(args)})"
        r = self.ret(fn, rt)
        body = [*pre, f"{rt} r = {call};" if r else f"{call};", *post]
        if r:
            body.append(r)
        return [f"HOST_CDECL(SDL2, {fn}) {{  // {rt} {fn}({', '.join(f'{t} {n}' for t, n in params)})",
                *(f"    {s}" for s in body), "}"]

    def converter(self, name):
        gsz, gf = self.g[name]
        _, hf = self.h[name]
        assert [f[2] for f in gf] == [f[2] for f in hf], name
        g2h, h2g = [], []
        for off, t, f in gf:
            if ":bits" in t or "[" in t and "*" in t:
                raise ValueError(f"{name}.{f}: unsupported field {t}")
            if base_of(t) is not None:
                g2h.append(f"    h->{f} = sdl_host_or_null(rd32(g + {off}));")
                h2g.append(f"    wr32(g + {off}, sdl_handle(h->{f}));")
            else:
                g2h.append(f"    memcpy(&h->{f}, P(g + {off}), sizeof h->{f});")
                h2g.append(f"    memcpy(P(g + {off}), &h->{f}, sizeof h->{f});")
        return [f"static void g2h_{name}(uint32_t g, {name} *h) {{", *g2h, "}",
                f"static void h2g_{name}(const {name} *h, uint32_t g) {{", *h2g, "}"]

    def check_event(self):
        """The SDL_Event members whose layout differs must be exactly the ones sdl2.c converts."""
        members = {t.removeprefix("struct ").split()[0] for _, t, _ in self.g["SDL_Event"][1]}
        differ = {m for m in members if m in self.g and not self.same(m)}
        if differ != EVENT_HAND:
            sys.exit(f"SDL_Event members with differing layouts changed: {sorted(differ ^ EVENT_HAND)}")
        if self.g["SDL_Event"][0] != self.h["SDL_Event"][0]:
            sys.exit("SDL_Event size differs")


def generate():
    """Write sdl2_gen.c and sdl2_layout.h; returns (bridged count, list of imports)."""
    gen = Gen()
    gen.check_event()
    imports = sorted({n for d, n in load("noita").imports.values() if d.upper() == "SDL2.DLL"})
    todo = sorted(set(imports) - HLE - HAND | EXTRA)
    missing = [f for f in todo if f not in gen.decls]
    if missing:
        sys.exit(f"not declared in the SDL2 headers: {missing}")
    thunks, errors = [], []
    for fn in todo:
        try:
            thunks += gen.thunk(fn)
        except ValueError as e:
            errors.append(f"{fn}: {e}")
    if errors:
        sys.exit("can't marshal:\n  " + "\n  ".join(errors))
    conv = [line for name in sorted(gen.converters) for line in gen.converter(name)]
    GEN.mkdir(parents=True, exist_ok=True)
    (GEN / "sdl2_gen.c").write_text("\n".join([
        "// Generated by tools/gen_sdl.py from the SDL2 headers: host thunks for the game's SDL2 imports.",
        "#include <string.h>", '#include "hle.h"', '#include "sdl2.h"', "", *conv, "", *thunks, ""]))
    lay = ["// Generated by tools/gen_sdl.py: guest (i686 MSVC) layouts of the SDL2 structs.", "#pragma once"]
    for name, (size, fields) in sorted(gen.g.items()):
        lay.append(f"#define G_{name}_SIZE {size}")
        lay += [f"#define G_{name}_{f} {off}" for off, _, f in fields]
    (GEN / "sdl2_layout.h").write_text("\n".join(lay) + "\n")
    return len(todo), imports


def main():
    n, imports = generate()
    print(f"SDL2: {len(imports)} imports: {n} bridged ({len(EXTRA)} extra for tests), "
          f"{len(HAND)} hand-written, {len(HLE)} HLE; wrote {GEN / 'sdl2_gen.c'}")


if __name__ == "__main__":
    main()
