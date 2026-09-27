"""GL survey (input for the generated GL bridge): what noita.exe calls, what its shaders need, what macOS has.

  1. Extracts data/shaders/*.{frag,vert} from data.wak into build/shaders/ and reports their GLSL #versions and
     the fixed-function built-ins they use.
     data.wak: 16-byte header (u32 0, u32 file count, u32 end of the file table = offset of the first file's data,
     u32 0), then per file (u32 data offset from the start of the wak, u32 size, u32 name length, name bytes: a
     '/'-separated path like "data/shaders/common.frag", no terminator), then the file data, uncompressed.
  2. Reads a call-count file from a game run (MINA4MAC_COUNT_IMPORTS=<file> build/mina4mac) and lists the opengl32
     names that were called, with counts, and how many were only looked up (GetProcAddress) or imported.
  3. Checks every called and looked-up name against the macOS SDK headers: OpenGL/gl.h + glext.h for the legacy
     2.1 context, OpenGL/gl3.h + gl3ext.h for the core 4.1 context.

  uv run tools/glsurvey.py [counts file]   (default build/import_counts.tsv)
"""
import re
import struct
import subprocess
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GAME = ROOT / "build" / "game"
OUT = ROOT / "build" / "shaders"
# Built-ins of GLSL 1.10 that the core profile (GLSL 1.40+ core) removed.
FIXED = r"\b(gl_(?:FragColor|FragData|TexCoord|Color|SecondaryColor|FrontColor|BackColor|Vertex|Normal|MultiTexCoord\d|"
FIXED += r"ModelViewMatrix|ProjectionMatrix|ModelViewProjectionMatrix|TextureMatrix|NormalMatrix|FogCoord|LightSource\w*)"
FIXED += r"|ftransform|texture(?:1D|2D|3D|Cube)(?:Proj)?(?:Lod)?|shadow2D\w*|attribute|varying)\b"


def wak_files(path):
    d = path.read_bytes()
    n, table_end = struct.unpack_from("<II", d, 4)
    o = 16
    for _ in range(n):
        off, size, nlen = struct.unpack_from("<3I", d, o)
        yield d[o + 12 : o + 12 + nlen].decode(), d[off : off + size]
        o += 12 + nlen
    assert o == table_end, (o, table_end)


def header_names(*headers):
    sdk = subprocess.run(["xcrun", "--show-sdk-path"], capture_output=True, text=True, check=True).stdout.strip()
    names = set()
    for h in headers:
        text = (Path(sdk) / "System/Library/Frameworks/OpenGL.framework/Headers" / h).read_text(errors="replace")
        names |= set(re.findall(r"\b(gl[A-Z]\w*)\s*\(", text))
    return names


def main():
    counts_path = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "build" / "import_counts.tsv"

    OUT.mkdir(parents=True, exist_ok=True)
    versions, fixed = Counter(), Counter()
    for name, data in wak_files(GAME / "data" / "data.wak"):
        if not (name.startswith("data/shaders/") and name.endswith((".frag", ".vert"))):
            continue
        (OUT / name.removeprefix("data/shaders/")).write_bytes(data)
        src = re.sub(r"//[^\n]*|/\*.*?\*/", "", data.decode("latin1"), flags=re.S)
        v = re.search(r"#version\s+(\d+)", src)
        versions[v.group(1) if v else "none"] += 1
        fixed.update(set(re.findall(FIXED, src)))
    print(f"shaders -> {OUT.relative_to(ROOT)}: #version {dict(versions)}")
    print("fixed-function built-ins (shaders using each):", ", ".join(f"{k} {n}" for k, n in fixed.most_common()))

    rows = [line.split("\t") for line in counts_path.read_text().splitlines()]
    gl = {n.split("!", 1)[1]: int(c) for c, n in rows if n.lower().startswith("opengl32.dll!")}
    called = sorted((n for n in gl if gl[n]), key=lambda n: -gl[n])
    print(f"\nopengl32 names: {len(gl)} thunks (imported or looked up), {len(called)} called")
    for n in called:
        print(f"  {gl[n]:>12,}  {n}")

    legacy, core = header_names("gl.h", "glext.h"), header_names("gl3.h", "gl3ext.h")
    wgl = {n for n in gl if n.startswith("wgl")}
    for label, have in (("legacy 2.1", legacy), ("core 4.1", core)):
        miss_called = [n for n in called if n not in have and n not in wgl]
        miss_all = sorted(n for n in gl if n not in have and n not in wgl)
        print(f"\n{label}: {len(miss_called)} called names missing: {' '.join(miss_called) or '-'}")
        print(f"{label}: {len(miss_all)} of {len(gl) - len(wgl)} looked-up names missing: {' '.join(miss_all)}")
    print(f"\nwgl names: {' '.join(sorted(wgl)) or '-'}")


if __name__ == "__main__":
    main()
