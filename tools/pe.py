"""Load the game's PE modules (from the user's own install) at their chosen guest bases and expose
sections, relocations, imports and exports."""
import os
import struct
from functools import cached_property
from pathlib import Path

import pefile

# Gitignored copy of the game binaries from the user's own install (see PLAN.md).
DEFAULT_GAME_DIR = Path(__file__).parent.parent / "build/game"


ROOT = Path(__file__).parent.parent

# The module list: short name -> (file in the game dir, guest load base). This is the one place load
# bases are chosen. They must not overlap each other, the guest heap (0x20000000-0xE0000000), host
# thunks (0xF0000000+) or the difftest stack/code/scratch areas (0x08000000-0x10040000). noita.exe
# keeps its preferred base; the DLLs all prefer 0x10000000 and are rebased via their relocations.
MODULES = {
    "noita": ("noita.exe", 0x00400000),        # size 0xee7000
    "msvcp120": ("msvcp120.dll", 0x18000000),  # size 0x71000
    "msvcr120": ("msvcr120.dll", 0x18100000),  # size 0xee000
    "lua51": ("lua51.dll", 0x18200000),        # size 0x5a000
    "fmod": ("fmod.dll", 0x18300000),          # size 0x1bc000
    "fmodstudio": ("fmodstudio.dll", 0x18500000),  # size 0x114000
    "Galaxy": ("Galaxy.dll", 0x18700000),      # size 0x9c7000
    "SDL2": ("SDL2.dll", 0x19100000),          # size 0xfd000
}


def game_dir() -> Path:
    return Path(os.environ.get("NOITA_DIR", DEFAULT_GAME_DIR))


def build_dir(module: str) -> Path:
    """Where a module's outputs (discover.pkl, survey.pkl, image.bin, generated C) go."""
    return ROOT / "build" / module


class Image:
    """A PE image mapped at `base` (default: its preferred base), with relocations applied."""

    def __init__(self, path: Path, base: int | None = None):
        self.path = path
        self.pe = pefile.PE(str(path), fast_load=False)
        self.pref_base = self.pe.OPTIONAL_HEADER.ImageBase
        self.base = self.pref_base if base is None else base
        self.entry = self.base + self.pe.OPTIONAL_HEADER.AddressOfEntryPoint
        self.sections = []
        for s in self.pe.sections:
            name = s.Name.rstrip(b"\0").decode()
            va = self.base + s.VirtualAddress
            self.sections.append((name, va, va + max(s.Misc_VirtualSize, s.SizeOfRawData), s))
        self.mem = self.pe.get_memory_mapped_image()
        delta = (self.base - self.pref_base) & 0xFFFFFFFF
        if delta:
            mem = bytearray(self.mem)
            for site in self.relocs:
                off = site - self.base
                v = struct.unpack_from("<I", mem, off)[0]
                struct.pack_into("<I", mem, off, (v + delta) & 0xFFFFFFFF)
            self.mem = bytes(mem)

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
                out[imp.address - self.pref_base + self.base] = (dll, name)
        return out

    @cached_property
    def exports(self) -> dict[int, list[str]]:
        """Exported VA -> names (forwarders excluded)."""
        out = {}
        d = getattr(self.pe, "DIRECTORY_ENTRY_EXPORT", None)
        for e in d.symbols if d else []:
            if e.forwarder is None:
                out.setdefault(self.base + e.address, []).append(e.name.decode() if e.name else f"#{e.ordinal}")
        return out


def load(module="noita") -> Image:
    """Load a module from MODULES at its chosen base."""
    file, base = MODULES[module]
    return Image(game_dir() / file, base)
