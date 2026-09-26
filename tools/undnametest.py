"""Check runtime/undname.c against llvm-undname (Homebrew llvm) on every RTTI type name in noita.exe and
msvcp120.dll. llvm-undname's output is normalised to MSVC's type_info::name style (no space after commas,
"> >" for nested template closes, "(__cdecl*)", and anonymous namespaces by name). Names undname.c declines (local classes, pointers to
members) are counted, not failed; any other difference fails.

  uv run tools/undnametest.py [-v]
"""
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from pe import ROOT, game_dir  # noqa: E402

UNDNAME = "/opt/homebrew/opt/llvm/bin/llvm-undname"


def rtti_names():
    names = set()
    for f in ("noita.exe", "msvcp120.dll"):
        for m in re.finditer(rb"\.\?A[\x21-\x7e]+", (game_dir() / f).read_bytes()):
            if m.group().endswith(b"@"):  # skip stray strings that merely start like one
                names.add(m.group().decode())
    return sorted(names)


def normalise(s):
    s = s.removesuffix(" `RTTI Type Descriptor Name'").replace(", ", ",")
    while ">>" in s:
        s = s.replace(">>", "> >")
    s = re.sub(r"::0x[0-9a-f]+::", "::`anonymous namespace'::", s)  # llvm prints a backreferenced one by id
    return re.sub(r"\((__\w+) \*(const)?\)", r"(\1*\2)", s)


def main():
    names = rtti_names()
    exe = ROOT / "build/undnametest"
    subprocess.run(["clang", "-O2", "-Wall", "-Wextra", "-Werror", str(ROOT / "runtime/undname.c"),
                    str(ROOT / "runtime/undname_test.c"), "-o", str(exe)], check=True)
    ours = subprocess.run([str(exe)], input="\n".join(names) + "\n", capture_output=True, text=True,
                          check=True).stdout.splitlines()
    out = subprocess.run([UNDNAME, *names], capture_output=True, text=True).stdout
    ref = [normalise(r.strip().split("\n")[-1]) for r in out.split("\n\n") if r.strip()]
    assert len(ref) == len(names) == len(ours)
    ok = declined = bad = 0
    for n, o, r in zip(names, ours, ref):
        if o == "?":
            declined += 1
            if "-v" in sys.argv:
                print("declined", n)
        elif o == r:
            ok += 1
        else:
            bad += 1
            print(f"MISMATCH {n}\n  ours {o}\n  llvm {r}")
    print(f"undname: {ok} match, {declined} declined, {bad} mismatch of {len(names)}")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
