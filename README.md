# mina4mac

Native Noita on Apple Silicon, without Wine or Rosetta. `noita.exe` (32-bit x86) is statically recompiled to C
and compiled for ARM64, and a native runtime (`runtime/`, see its README) stands in for Windows and the game's
DLLs: SDL2, OpenGL, LuaJIT and FMOD are bridged to their macOS versions.

## What you need

- A Mac with Apple Silicon (M1 or later), macOS 14 or later, and a few GB of free disk space.
- Noita for Windows from GOG, the build of Jan 25 2025 (version `20250125-1640` on GOG). See [Supported builds](#supported-builds).
- About 15 minutes, most of it waiting for the build.

This repository contains no game code or assets, so there's no download-and-play app: you build `Noita.app`
yourself from your own copy of the game. The steps below take you through it. (The repository does include facts
observed in the binary: the runtime is written against addresses and data layouts in one build of `noita.exe`, and
`tools/icall_targets.txt` lists indirect call sites and the targets seen there at run time. The recompiled C is
generated on your Mac and is never committed or shipped.)

## Install

All commands go in Terminal (Applications → Utilities → Terminal).

### 1. Install the tools

Install Apple's command line developer tools (a dialog opens; click Install):

```sh
xcode-select --install
```

Install [Homebrew](https://brew.sh) if you don't have it (follow the instructions on its home page), then:

```sh
brew install sdl2 uv
```

### 2. Get mina4mac

```sh
git clone --recursive https://github.com/paulstraw/mina4mac.git
cd mina4mac
```

The remaining commands run from this `mina4mac` folder. If you cloned without `--recursive`, run
`git submodule update --init` once.

### 3. Get Noita's Windows files onto your Mac

You need the folder that contains `noita.exe` (along with `data/`, `msvcp120.dll` and the rest). There are two
ways to get it:

- **If you already play Noita through Wine** (CrossOver, Whisky, Porting Kit, a Wineskin/Sikarugir wrapper): the
  game is already installed inside the wrapper, usually at `drive_c/GOG Games/Noita`. For a wrapper app,
  right-click it in Finder, choose Show Package Contents, and look under `Contents/SharedSupport/prefix/` (or
  similar). Leave it where it is.
- **Otherwise**, unpack GOG's offline installer. On gog.com, open your library, select Noita, and under the
  Windows downloads choose the offline backup installer (`setup_noita_….exe`, plus any `.bin` files listed next
  to it; download them all to the same folder). Then:

  ```sh
  brew install innoextract
  innoextract --gog -d ~/Games/Noita ~/Downloads/setup_noita_*.exe
  ```

  The game folder is `~/Games/Noita`, or `~/Games/Noita/app` if innoextract put the files in an `app` subfolder:
  use whichever one contains `noita.exe`.

Keep this folder where it is: the app runs the game from it.

### 4. Set up sound (optional, recommended)

Noita uses the FMOD audio engine, and FMOD's license doesn't allow redistributing it, so you download it yourself.
Without it, the game runs silently. The game ships FMOD 2.01, so you need a 2.01 version (2.02 and later won't
work):

1. Create a free account at [fmod.com](https://www.fmod.com) (Sign in → Register) and sign in.
2. Go to [fmod.com/download](https://www.fmod.com/download) and find **FMOD Engine**. Pick version **2.01.23**
   from the version list (it's an older version, so you may have to open the older versions list); any 2.01.x
   works, but 2.01.23 is the one that's been tested.
3. Download the **Mac** installer. You get `fmodstudioapi20123mac-installer.dmg` in your Downloads folder.
4. Back in Terminal, in the `mina4mac` folder:

   ```sh
   tools/setup_fmod.sh
   ```

   It looks for that file in `~/Downloads`; if yours is elsewhere or a different 2.01 version, pass its path:
   `tools/setup_fmod.sh ~/path/to/fmodstudioapi201XXmac-installer.dmg`. It should finish with a line starting
   `FMOD 0x000201`.

If you skip this now, you can do it later and repeat step 5.

### 5. Build the app

Pass the game folder from step 3. (Tip: type the command up to the opening quote, then drag the folder from Finder
into the Terminal window to paste its path, and remove the extra quotes if it added its own.)

```sh
tools/package_app.sh "/path/to/Noita"
```

This checks that your `noita.exe` is the supported build, then recompiles it for your Mac. It takes about 5 minutes
on an M1 Max, and your fans may spin up. It ends with a line like
`build/Noita.app: game /path/to/Noita, FMOD yes`. (FMOD `no` means step 4 wasn't done: the game will be silent.)

### 6. Play

Copy the app to your Applications folder (or anywhere else) and open it. The name below keeps it from clashing
with a Wine wrapper that may already be called `Noita.app`:

```sh
ditto build/Noita.app ~/Applications/"Noita (mina4mac).app"
open ~/Applications/"Noita (mina4mac).app"
```

**Existing saves.** mina4mac keeps its own saves, laid out like a Windows profile, in
`~/Library/Application Support/mina4mac/AppData/LocalLow/Nolla_Games_Noita/`. To continue a run from Windows or
Wine, open the app once so that folder exists, quit it, and copy your old `Nolla_Games_Noita` folder's contents
there (from `%USERPROFILE%\AppData\LocalLow\` on Windows, or `drive_c/users/<name>/AppData/LocalLow/` in a Wine
prefix). Back up both sides first.

**Updating mina4mac.** In the `mina4mac` folder, run `git pull && git submodule update --init`, then repeat steps
5 and 6. Do the same if you move the game folder.

## Supported builds

Only the GOG build of Jan 25 2025 (listed on GOG as version `20250125-1640`) is supported, and step 5 refuses any other `noita.exe`. The Steam version is a
different binary and doesn't work. When Noita is patched, the runtime has to be updated for the new build before it
works, so keep a copy of the supported game folder (and its installer).

Tested on an M1 Max with macOS 26.6. Other chips and macOS versions haven't been tried yet, so please include yours
in bug reports.

## Troubleshooting

- **"… is not the supported build"**: your `noita.exe` is a different version (or the Steam build). See
  [Supported builds](#supported-builds).
- **"no Noita install at …"**: the path in step 5 isn't the folder that contains `noita.exe` and `data/`.
- **No sound**: step 4 wasn't done, or was done after step 5. Run `tools/setup_fmod.sh`, then steps 5 and 6 again.
- **"Noita install not found" when opening the app**: the game folder moved or was deleted. Run steps 5 and 6
  again with its new location.
- **Crashes or other bugs**: [open an issue](https://github.com/paulstraw/mina4mac/issues) with your Mac's chip
  and macOS version, and attach the log, `~/Library/Logs/mina4mac.log`. Earlier launches' logs are
  `mina4mac.log.1` (the one before) to `mina4mac.log.5`.

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
