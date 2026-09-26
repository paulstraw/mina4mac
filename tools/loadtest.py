"""PE loader test (runtime/load_test.c): map every module in pe.MODULES from the game directory with
rt_map_pe at its chosen base, and check it byte-for-byte against tools/pe.py's image. Every module but
noita.exe is rebased, so this covers relocations too.

  uv run tools/loadtest.py
"""
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from lift import ROOT  # noqa: E402
from pe import MODULES, build_dir, game_dir, load  # noqa: E402


def main():
    exe = ROOT / "build/loadtest"
    subprocess.run(["clang", "-O2", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "runtime"),
                    str(ROOT / "runtime/load_test.c"), str(ROOT / "runtime/rt.c"), "-o", str(exe)], check=True)
    ok = True
    for m, (file, base) in MODULES.items():
        img = load(m)
        out = build_dir(m) / "loadtest.bin"
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_bytes(bytes(img.mem))
        entry = base + img.pe.OPTIONAL_HEADER.AddressOfEntryPoint
        ok &= subprocess.run([str(exe), str(game_dir() / file), f"{base:x}", f"{entry:x}", str(out)]).returncode == 0
    print("loadtest:", "ok" if ok else "FAIL")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
