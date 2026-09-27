# noitamac

Native Noita on Apple Silicon, without Wine or Rosetta. `noita.exe` (32-bit x86) is statically recompiled to C
and compiled for ARM64, and a native runtime (`runtime/`, see its README) stands in for Windows and the game's
DLLs: SDL2, OpenGL, LuaJIT and FMOD are bridged to their macOS versions.

## Bring your own game

This repository contains no game files and no code derived from them. The recompiled C is generated from your
copy of the game, so it is never committed or shipped. You need:

- Noita for Windows, the GOG build of Jan 25 2025 (`noita.exe` sha256 `f0f5ffd1…`; the runtime is written
  against that one build, and the packaging script checks it). Any directory with the installed game works,
  for example a Wine prefix's `drive_c/GOG Games/Noita`.
- Xcode's command line tools, [uv](https://docs.astral.sh/uv/), and Homebrew's `sdl2`.
- LuaJIT, as a submodule: `git submodule update --init`.
- Optional, for sound: the FMOD Engine for Mac 2.01.x from fmod.com (it needs an account), installed with
  `tools/setup_fmod.sh <dmg>`. Without it the game runs silently.

## Build Noita.app

```sh
tools/package_app.sh "/path/to/GOG Games/Noita"
```

This runs the discover/lift/build pipeline locally against your install's `noita.exe` and `msvcp120.dll`
(about 3–4 minutes), then writes `build/Noita.app`. Copy the app wherever you like. It bundles SDL2 and FMOD
(if set up), but not the game: it runs the game from your install, which must stay where it is. If you move the
install, run the script again. Saves are in `~/Library/Application Support/noitamac/`, and the log is
`~/Library/Logs/noitamac.log`.

## Development

`PLAN.md` and `PLAN2.md` describe the design and record the history. The tools work on a copy of the game in
`build/game`: copy the binaries there, then `tools/setup_game.sh` adds `data/` and `mods/`.
`uv run tools/build_all.py` builds `build/noitamac`, and `tools/check.sh` runs the verification sequence.
