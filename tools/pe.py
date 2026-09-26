"""Load noita.exe (from the user's own install) and expose sections, relocations and imports."""
import os
import struct
from functools import cached_property
from pathlib import Path

import pefile

# Gitignored copy of the game binaries from the user's own install (see PLAN.md).
DEFAULT_GAME_DIR = Path(__file__).parent.parent / "build/game"


def game_dir() -> Path:
    return Path(os.environ.get("NOITA_DIR", DEFAULT_GAME_DIR))


class Image:
    def __init__(self, path: Path):
        self.path = path
        self.pe = pefile.PE(str(path), fast_load=False)
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.entry = self.base + self.pe.OPTIONAL_HEADER.AddressOfEntryPoint
        self.sections = []
        for s in self.pe.sections:
            name = s.Name.rstrip(b"\0").decode()
            va = self.base + s.VirtualAddress
            self.sections.append((name, va, va + max(s.Misc_VirtualSize, s.SizeOfRawData), s))
        self.mem = self.pe.get_memory_mapped_image()

    def section(self, name):
        for n, lo, hi, s in self.sections:
            if n == name:
                return lo, hi
        raise KeyError(name)

    def section_of(self, va):
        for n, lo, hi, _ in self.sections:
            if lo <= va < hi:
                return n
        return None

    def read(self, va, n):
        off = va - self.base
        return self.mem[off:off + n]

    def u32(self, va):
        return struct.unpack_from("<I", self.mem, va - self.base)[0]

    @cached_property
    def relocs(self) -> list[int]:
        """VAs of every absolute 32-bit address stored in the image (HIGHLOW relocations)."""
        out = []
        for block in getattr(self.pe, "DIRECTORY_ENTRY_BASERELOC", []):
            for e in block.entries:
                if e.type == 3:
                    out.append(self.base + e.rva)
        return sorted(out)

    @cached_property
    def imports(self) -> dict[int, tuple[str, str]]:
        """IAT slot VA -> (dll, symbol)."""
        out = {}
        for d in self.pe.DIRECTORY_ENTRY_IMPORT:
            dll = d.dll.decode()
            for imp in d.imports:
                name = imp.name.decode() if imp.name else f"#{imp.ordinal}"
                out[imp.address] = (dll, name)
        return out


def load(name="noita.exe") -> Image:
    return Image(game_dir() / name)
