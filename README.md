# mina4mac

Native Noita on Apple Silicon, without Wine or Rosetta. `noita.exe` (32-bit x86) is statically recompiled to C
and compiled for ARM64, and a native runtime (`runtime/`, see its README) stands in for Windows and the game's
DLLs: SDL2, OpenGL, LuaJIT and FMOD are bridged to their macOS versions.

## Bring your own game

This repository contains no game code or assets. It does include facts observed in the binary: the runtime is
written against addresses and data layouts in one build of `noita.exe`, and `tools/icall_targets.txt` lists
indirect call sites and the targets seen there at run time. The recompiled C is generated from your copy of the
game, so it is never committed or shipped. You need:

- Noita for Windows, the GOG build of Jan 25 2025 (`noita.exe` sha256 `f0f5ffd1…`; the runtime is written
  against that one build, and the packaging script checks it). Any directory with the installed game works,
  for example a Wine prefix's `drive_c/GOG Games/Noita`.
- Xcode's command line tools, [uv](https://docs.astral.sh/uv/), and Homebrew's `sdl2`.
- LuaJIT, as a submodule: `git submodule update --init`.
- Optional, for sound: the FMOD Engine for Mac 2.01.x from fmod.com (it needs an account), installed with
  `tools/setup_fmod.sh <dmg>`. Without it the game runs silently.

### Supported builds

Only the GOG build of Jan 25 2025 is supported, and the packaging script refuses any other `noita.exe`. The
Steam version is a different binary and doesn't work. When Noita is patched, the runtime has to be updated for the
new build before it works, so keep a copy of the supported install.

Tested on an M1 Max with macOS 26.6. The app requires macOS 14 or later and Apple Silicon; other chips and macOS
versions haven't been tried yet, so please include yours in bug reports.

## Build Noita.app

```sh
tools/package_app.sh "/path/to/GOG Games/Noita"
```

This runs the discover/lift/build pipeline locally against your install's `noita.exe` and `msvcp120.dll`
(about 5 minutes on an M1 Max), then writes `build/Noita.app`. Copy the app wherever you like. It bundles SDL2 and FMOD
(if set up), but not the game: it runs the game from your install, which must stay where it is. If you move the
install, run the script again. Saves are in `~/Library/Application Support/mina4mac/`, and the log is
`~/Library/Logs/mina4mac.log` (the previous launch's is `mina4mac.log.1`).

## Known issues

- Sound needs the FMOD Engine, which fmod.com only offers behind a (free) account, so the app can't set it up for
  you; without it the game is silent.

## Development

`docs/PLAN.md` and `docs/PLAN2.md` describe the design and record the history. The tools work on a copy of the game in
`build/game`: copy the binaries there, then `tools/setup_game.sh` adds `data/` and `mods/`.
`uv run tools/build_all.py` builds `build/mina4mac` with ThinLTO (about 3 minutes; `--lto off` skips it), and
`tools/check.sh` runs the verification sequence. `tools/determinism.sh run` checks world generation against the Wine
build without clicks. The build first runs `tools/regsum.py`, which proves per function which callee-saved registers
it preserves and how many argument bytes it pops, so call sites can skip reloads (cached in `build/<module>/regsum.pkl`;
about 2 minutes when the lifter changes). `build_all.py --sync check` builds a binary that verifies every skipped
reload at run time and exits 12 on a wrong summary; `--sync full` turns the optimization off.

Optional profile-guided build: `tools/pgo.sh` builds an instrumented `build/mina4mac`, trains it on the perfbench
scenes (`tools/perfbench.sh install mina4mac` first; about 10 minutes, since the instrumented game runs ~12× slower),
merges the profile into `build/pgo/mina4mac.profdata`, writes an order file (hottest functions first) and rebuilds
with both (`tools/build_all.py --pgo use --order`). The profile is derived from your own copy of the game, so it
stays in `build/` and is never committed. The gain is small (see `docs/PLAN3.md`, Phase 10), and the default build doesn't use it.

## License

MIT, see `LICENSE`. Bundled third-party code keeps its own license: LuaJIT (`third_party/luajit`, MIT) and the
Khronos OpenGL registry (`third_party/khronos/gl.xml`, Apache-2.0). SDL2 (zlib) and, if you set it up, FMOD (under
FMOD's own license) are copied into the app you build. Noita is © Nolla Games; this project isn't affiliated with them.
