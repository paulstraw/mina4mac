# noitamac, part 2: from first window to a playable game

This continues `PLAN.md`, which is complete: the recompiled game runs through startup to a black window
with a GL context. **Read the Context section of `PLAN.md` first.** Its goal, legal and repository rules,
toolchain, codegen conventions, verification sequence (`tools/check.sh`) and working rules all still
apply. Also read the task notes of Phases 2–3 there; they describe the runtime this plan builds on.

## Context

### Where Phase 3 stopped

- The window "Noita - Build Jan 25 2025 - 16:31:25" opens (2560x1440, swap interval 1) and stays black.
- Startup stops at `fmodstudio!FMOD::Studio::System::create` (FMOD 2.01.05), called from 0x47a6fb.
- `build/game` has only the binaries, so the logger reports missing `data/ui_gfx` and translations.
- `opengl32.c` is a placeholder. GetProcAddress returns a `dll!name` thunk for any name, and about 12
  GL 1.1 calls are forwarded, only while a CGL context is current.
- Keep the display awake while running the game (`caffeinate -u`). Otherwise `SDL_GL_SwapWindow` blocks
  forever on vsync.

### Facts gathered while writing this plan

- **Game install contents.** The source install (see PLAN.md) holds `data/` (1.4 GB: `data.wak`, `audio/`,
  `fonts/`, `schemas/`, `translations/`, `video/`), `mods/` (7.7 MB) and `config.xml`, alongside the
  binaries.
- **FMOD surface.** The exe imports 35 `fmodstudio` and 7 `fmod` functions. All are C++ API calls with
  the `__stdcall` convention (`SG`/`QAG`/`QBG` in the mangled names), so member functions take `this` as
  the first stack argument.
  - Studio: System (create, initialize, release, update, isValid, getCoreSystem, getBus, getEvent,
    loadBankFile, setCallback, setListenerAttributes), Bank (getEventCount, getEventList),
    EventDescription (createInstance, getLength, getMin/MaximumDistance, getPath, getUserProperty,
    isSnapshot, loadSampleData), EventInstance (start, stop, release, isValid, set3DAttributes,
    setParameterByName, setPaused, setVolume, setCallback, get/setUserData, getTimelinePosition), and
    Bus (setPaused, setVolume).
  - Core: System (getMasterChannelGroup, getVersion, set3DSettings, setDSPBufferSize, setOutput,
    setSoftwareChannels) and ChannelControl::setVolume.
  - The game chooses the output type itself through `System::setOutput`.
- **Lua surface.** 171 `lua51` imports. About 60 are the public API: `luaL_newstate`, `lua_pcall`,
  `lua_pushcclosure`, `lua_tolstring`, `luaL_ref` and so on, plus `luaopen_*` for base, bit, debug, ffi,
  io, jit, math, os, package, string and table.
  - The rest are **LuaJIT internals** that a stock `lua51.dll` doesn't export: `lj_cf_os_*`,
    `lj_cf_ffi_*`, `lj_cf_debug_*`, `lj_cf_package_*`, `io_file_*`, `ll_load`, `setfenv` and so on.
    These are the functions the mod sandbox `VirtualProtect`s and overwrites with `mov [0],0`, so the
    game probably only takes their addresses.
  - The game's `lua51.dll` is a custom build of LuaJIT 2.0.4.
- **Imports missing from PLAN.md's table:** `WS2_32` (12 imports: WSAStartup/Cleanup/GetLastError,
  socket, connect, send, recv, closesocket, gethostbyname, htons, ioctlsocket, setsockopt), probably for
  streaming integrations. Single imports: `USER32!GetActiveWindow`, `WINMM!timeBeginPeriod`,
  `ole32!CoTaskMemFree`, `COMDLG32!GetOpenFileNameA`.

### Key design decisions (made here; revisit only with evidence)

- **Audio: bridge to native macOS FMOD, don't recompile it.** The surface is only 42 functions with
  simple types. A recompiled `fmod.dll` would also need WASAPI/COM emulation.
  - The macOS FMOD Studio API must be downloaded by the user from fmod.com (it needs an account; it's
    bring-your-own, like the exe). Never commit it.
  - Until then, a silent stub unblocks everything else.
- **Lua: host ARM64 LuaJIT 2.1 with the JIT enabled, not recompiled `lua51.dll`.**
  - Lua is a large share of Noita's per-frame work, and a recompiled LuaJIT could only run as an
    interpreter because its JIT emits x86.
  - Every LuaJIT object is allocated from the guest heap via `lua_newstate(alloc)` (supported in GC64
    mode), so every pointer the game sees is a 32-bit guest address (`host − MEM`).
  - Fallback if the bridge fails: recompile `lua51.dll` with the JIT disabled. Record the reason if we
    switch.
- **OpenGL: a bridge generated from Khronos `gl.xml`.** Use the legacy 2.1 or the core 4.1 profile,
  whichever the GL survey shows the renderer needs.

## Tasks

### Phase 4: past startup

- [x] Game data. Add `tools/setup_game.sh`, which rsyncs the non-binary game files (`data/`, `mods/`,
  `config.xml`, and anything else the logger asks for) from the source install (overridable) into
  `build/game`. It must be idempotent, and it must never cause anything under `build/` to be committed.
  Then fix the file-API gaps until the logger stops reporting missing `data/ui_gfx` and translations.
  - Record in the note which files the game opens (including how it reads `data.wak`) and any path
    translation issues (`Z:\`, case sensitivity, `/` vs `\`).
  - `tools/setup_game.sh [src]` (`NOITA_SRC`/`NOITA_DIR`) rsyncs data/, mods/, config.xml and the _branch/_version/_release_notes/screenshot_paths files; refuses an un-ignored destination in the repo. No file-API gaps: logger is clean. Opens (`NOITAMAC_TRACE_FILES=1`): `data/data.wak` twice (via msvcp `_wfsopen`; all of `data/…` incl. ui_gfx/translations comes from it, read whole through an inlined `basic_filebuf` = 42.5M `fgetc` calls, ~1 s, since our FILE has no guest buffer), `data/icon.bmp`, config.xml, `mods\*` (FindFirstFileW) + each mod's mod.xml/compatibility.xml/settings.lua, LocalLow save_shared/config.xml and save00/mod_*. Paths are relative with `/` or absolute `Z:\…` with `\`; APFS is case-insensitive, so no case issues.
  - Also fixed on the way: x87 lifter bugs only msvcp120 hits (`fxch st(i)` was a no-op; one-operand `fadd/fmul/fdiv st(i)` wrote st(i), not st(0)) → stack-cookie failure in float parsing; new `insntest.py --x87reg` (x87regtest in check.sh). opengl32.c forwards glGen/Delete/BindTexture, glTexParameteri, glTex(Sub)Image2D. check.sh seed 1357 all ok; launcher again stops at `fmodstudio!FMOD::Studio::System::create` from 0x47a6fb.
- [x] FMOD silent stub (`runtime/fmod_stub.c`). Implement all 42 imports with `HOST(...)` and the
  mangled names.
  - Handles are small guest-heap objects so `isValid` and `get/setUserData` behave correctly.
  - Every function returns `FMOD_OK`, and callbacks never fire.
  - `getEventCount` and `getEventList` return real-looking empty data, or whatever the game needs to
    carry on. Read the call sites to decide.
  - Keep the stub selectable later with `NOITAMAC_AUDIO=stub` once real audio exists.
  - Record where startup stops next.
  - Handles are 16-byte {magic, kind, userdata, name}; banks/buses/descriptions are one per path. getVersion must report ≥0x20105 (checked at 0x47a738). Banks have 0 events, so getEventList is skipped. getUserProperty returns 74 (EVENT_NOTFOUND), which the game treats as "absent"; getEvent also accepts 74 but returns OK. Startup: create/initialize, 2 banks, 7 buses, update. envtest 195 checks; check.sh seed 22320 all ok.
  - Next stop: `opengl32!glCreateShader` from 0xdd64f1, before any `lua51` call. The next task needs GL shader stand-ins (or the Phase 6 bridge) first.
- [x] Run to the first `lua51` call, then inventory how the game uses Lua, to feed Phase 5.
  - Which of the 171 imports are called and which are only address-taken? Log the address-taken ones
    (the sandbox patches) together with the code that patches them.
  - How many `lua_State`s are created, on which threads, and with which `luaopen_*` libraries?
  - Which lua functions return floats in st0, and which take or return structs (`lua_Debug`)?
  - Does anything read LuaJIT object internals directly (for example `L->top`) instead of going through
    the API?
  - Record the answers in the note. Do not implement Lua yet.
  - No Lua before the menu, which now renders fully (opengl32.c forwards ~80 GL 2.1 calls: shaders, buffers, client/attrib arrays with buffer-offset handling, ARB FBOs; plus timeBeginPeriod, FileTimeToSystemTime/SystemTimeToTzSpecificLocalTime/GetLocalTime, null exception_ptr, mbstowcs_s, __crtSleep, offline wininet.c, failing FindFirstChangeNotificationW). New `NOITAMAC_CLICKS="t:x,y;…"` (sdl2.c; CGEvent posting is blocked without Accessibility) clicks New Game + first mode; check.sh uses it and stops at `luaL_newstate` from 0x7ed89e on a job-system worker thread (tid 0x130, via 0x849bc0 → 0x6afaa0 → 0x832dc0 → 0x7ed620). check.sh seed 31337 all ok, envtest 212.
  - `uv run tools/luasurvey.py` has the details. 50 imports are called; 121 (+ luaL_openlibs) are only stored into the sandbox list of 0x7ee720(cl=patch): per function VirtualProtect(16) and write `C7 05 00000000 00000000` (mov [0],0), originals saved once in a VirtualAlloc buffer and restored with cl=0 (callers 0x836e70/0x6daf60 patch, 0x9a0980 restores). All states come from 0x7ed880 (1 luaL_newstate site, called via 0x7ed620 from 15 places: created and lua_closed dynamically, likely per LuaComponent), on worker threads, so the bridge needs per-thread CPUs. It runs luaL_openlibs if [this+0x4e] (unsafe mods), else pushes the *thunks* of luaopen_base/table/string/math/bit/jit via lua_pushcclosure + lua_call (the bridge must map lua51 thunks to host functions), nils load/loadfile/loadstring/gcinfo/collectgarbage and defines dofile/dofile_once in Lua. st0: lua_tonumber (356 sites); lua_pushnumber takes a double arg. lua_Debug (32-bit, 100 bytes; name/what/source are pointers, which must point into guest memory) is used by the exe's own luaL_traceback copy 0x7ec490 (reads currentline +20, short_src +36) and 0x7ec2a0/0x7ec350. All 109 pushcclosure calls use 0 upvalues; the only pseudo-indices are GLOBALSINDEX and REGISTRYINDEX; no lua_error/luaL_error/userdata imports; lua_topointer (41 sites) and lua_tolstring pointers must be guest addresses. No lua_State field reads were found in the 401 registered C functions.

### Phase 5: LuaJIT

- [x] Host LuaJIT bridge (`runtime/lua51.c`).
  - Pin LuaJIT 2.1 as a git submodule under `third_party/` (MIT licensed, so it's fine to commit) and
    build it with the ARM64 JIT enabled.
  - `luaL_newstate` → `lua_newstate` with an allocator backed by the guest heap. Assert that every
    pointer handed to the guest lies inside guest memory.
  - Pointers: strings and buffers from the guest are `MEM + p`, and pointers returned to the guest are
    `host − MEM`.
    - Light userdata stays the raw 32-bit value; don't translate it.
    - `lua_touserdata` must convert full userdata, but not light userdata.
  - C closures: `lua_pushcclosure(L, guestfn, n)` pushes one host trampoline with the guest function as
    a hidden first upvalue. Every index argument must shift `lua_upvalueindex(k)` (`-10002 − k`) by one.
    The trampoline calls the guest function on the current thread's `CPU` and returns eax.
  - Errors: `lua_error` from a guest C function unwinds through recompiled frames.
    - The bridge's `lua_pcall`/`lua_call` saves and restores guest `esp` and `fs:[0]`.
    - Confirm that the lifted code has unwind tables so LuaJIT's external unwinder can walk it.
  - Sandbox: reproduce the effect of the game's `mov [0],0` patches, based on the Phase 4 inventory.
    For example, a write to a patched function's thunk marks it disabled, and the host function then
    raises a Lua error or aborts, like the original.
  - Add `runtime/lua_test.c` (the luatest step in check.sh) covering:
    - state creation and all `luaopen_*` libraries
    - guest closures with upvalues
    - errors raised in guest code and caught by `lua_pcall`, nested three deep
    - userdata round trips
    - `luaL_ref`
    - `lua_tonumber` returning in st0
    - a JIT-hot loop
  - LuaJIT v2.1 @ c6ffc14 (`third_party/luajit`); build_all rsyncs it to `build/luajit/` and makes a static amalg lib (GC64, external unwinding); lua51.c needs `-fexceptions` (its trampoline restores ebx/esi/edi/ebp/esp/fs:[0]/x87 in a cleanup when a Lua error abandons guest frames, e.g. Lua `pcall` of guest code). Every lifted function has a compact-unwind entry. `rt_thread_cpu` (proc.h) is the per-thread CPU. Sandbox is per library: patched = its luaopen_* thunk holds `mov [0],0`; opening it or calling any wrapped C function then exits 10. Guest C++ destructors in abandoned frames don't run.
  - luatest (check.sh): 48 checks, JIT loop 1.7 ns/iter with traces, 36 ns per Lua→guest call. check.sh seed 27182 all ok. Launcher: 4 states, 20 chunks loaded, 13 lua_pcalls, then stops at `MSVCR120!??_V@YAXPAX@Z` (delete[]) from 0x8b0858.
- [x] Run until the game's Lua init finishes (built-in scripts and mod loading with the default mods).
  Record which scripts ran, any API gaps, and where execution stops.
  - Lua init completes; nothing stops. It was unblocked by 1aa439e (delete[], _setjmp3, GetSystemTime, ConcRT id, glPush/PopAttrib) and 4093f73 (lua_tointeger), and runs in-game (Holy Mountain at 90 s). Traced New Game run (`NOITAMAC_TRACE_LUA=1`, check.sh clicks, default mods, all disabled so no mod scripts; log `build/luainit.log`): about 160 states, 1182 chunk loads, 0 pcall errors, no unimplemented imports. First is `data/scripts/init.lua` (utilities, biome_modifiers), then per-state `data/scripts/biomes/*` (191), director_helpers (145), item_spawnlists/biome_scripts (122 each), lib (89), gun (84), perks, items, game_helpers, static_tile, streaming_integration, status_effects.

### Phase 6: OpenGL

- [x] GL survey, before writing the bridge.
  - Count at runtime every GL name the game actually calls (the thunk path), not just the 1,037 it looks
    up.
  - Extract the shaders from `data.wak` (document the wak format in the note) and record their GLSL
    `#version`s and any fixed-function use.
  - Decide between the legacy 2.1 and the core 4.1 context. Record the list of names that macOS lacks.
  - **Legacy 2.1.** `NOITAMAC_COUNT_IMPORTS=<file>` (new, rt.c) writes calls per thunk at exit; `uv run tools/glsurvey.py [file]` extracts shaders to `build/shaders/` and checks names against the SDK headers (gl.h+glext.h vs gl3.h+gl3ext.h). data.wak: header {0, count, table end, 0}, then per file {u32 offset, u32 size, u32 namelen, name}, then uncompressed data. All 23 shaders are `#version 110` (plus the included common.frag) and use gl_TexCoord/gl_FragColor/texture2D/gl_Color/gl_ModelViewMatrix/gl_MultiTexCoord; no GLSL in the exe. In a 150 s menu → New Game run, 50 of 1048 opengl32 thunks were called (+4 wgl*). Legacy lacks none of them, but core lacks 12 (client arrays, matrix stack, Push/PopAttrib, Ortho, Scalef). 461 looked-up names aren't in the legacy headers (mostly 3.x/4.x and DSA; see build/glsurvey.log). Options and fullscreen weren't exercised.
- [x] Generate the GL bridge (`tools/gen_gl.py` from `gl.xml`, output under `build/gen_all/`) for the
  names the survey found, all `__stdcall`.
  - Pointer arguments are guest pointers (`MEM + p`). But `gl*Pointer`, `glDrawElements` indices and
    `glTex*Image` with a bound pixel-unpack buffer take buffer *offsets* when a buffer is bound, and those
    must not be translated.
  - `glGetString` copies into guest memory.
  - `glMapBuffer*` returns a guest shadow buffer that's copied back on unmap.
  - GetProcAddress returns NULL for names macOS doesn't provide, as a real driver would.
  - Replace the no-context workaround in `opengl32.c`.
  - Add a gltest step with a hidden window: clear, draw a triangle and `glReadPixels` through the thunks.
  - `gen_gl.py` (run by build_all; gl.xml committed at `third_party/khronos/`, OpenGL-Registry @ 1cdd228e) bridges the 583 of noita.exe's 1,044 GL names that the SDK legacy headers declare → `build/gen_all/gl_gen.c` (575 generated, calls type-checked against the SDK prototypes; 8 hand-written in `runtime/opengl32.c`: GetString, ShaderSource, Map/UnmapBuffer + GetBufferPointerv shadow, MultiDrawElements*, DeleteSync; GLsync = guest handles; 40 array/element/pack/unpack pointer args become offsets while a buffer is bound). No-context no-op now lives in each thunk (`GL_CTX`, `opengl32.h`). `gl_proc_address`: GetProcAddress **and wglGetProcAddress** (the loader falls back to it on NULL, 922 calls) return NULL for the other 461. `runtime/gl_test.c` (gltest in check.sh, 43 checks, draws into an FBO); envtest links gl_gen.c. check.sh seed 4711 all ok; launcher again runs 300 s without stopping and the main menu renders (`build/gl_ingame.png`). Note the check.sh clicks miss New Game once a save exists ("Continue" shifts the menu).
- [x] First frame. Run until the main menu renders. Save `build/menu.png` with `tools/screenshot.sh` and
  fix blockers along the way (including C++ exceptions or `longjmp` if they're hit). Record the blockers.
  - No new blockers: `tools/screenshot.sh build/menu.png 20` (under `caffeinate -u`) captures the full menu (logo, Continue…Quit). The blockers were fixed in earlier tasks (Phase 4 notes, 1aa439e, the GL bridge). C++ exceptions weren't hit; `longjmp` still exits 11 but isn't reached on the way to the menu.

### Phase 7: playable

- [x] Play a run.
  - 2026-09-26, user playtest (the loop's launches, no timeout needed): the first level is playable. Moving,
    shooting and killing enemies work, it felt smooth with no slowdown, and no crash was seen (the run ended
    because the agent killed the process). Second session: Continue works (it restored a player made by
    the `starting_loadouts` mod, correctly), the mod's Lua and custom player sprite work, death → progress
    screen → death menu → new game works, windowed → fullscreen works, and SIGTERM exits 0 after the game
    saves its config. Options/rebinding and a 30+ minute session moved to the last task (they need a person).
  - **Fixed: missing Holy Mountain items and start-area cart.** Perks spawned but hearts, spell refresh,
    shop wands/spells, the workshop (wand editing) and the starting cart didn't. Perks come from the built-in
    `data/scripts/wang_scripts.csv`; the rest from Lua `RegisterSpawnFunction( 0xff6d934c, ... )`. Our
    `lua_tointeger` used cvttsd2si semantics, so every color above 0x7fffffff became 0x80000000 and the
    registrations collided. The game's lua51.dll (0x10007d50) uses lj_num2bit (round to nearest even, wrap
    modulo 2^32); the bridge now does the same, with luatest cases. Playtest confirmed: the cart spawns and two Holy Mountains were complete (hearts, refresh, shop, wand editing, perks).
  - **Fixed: fullscreen → windowed** came back as a huge, mostly black window with the game in the bottom-left
    corner. The game calls `SDL_SetWindowSize(1280,720)`, then `SDL_SetWindowFullscreen(0)`, then sets glViewport
    right away (0xdd7647/0xdd7654/0xdd7747). A macOS fullscreen Space leaves asynchronously and then restores its
    own frame (2560x1322). sdl2.c now sets `SDL_HINT_VIDEO_MAC_FULLSCREEN_SPACES=0` (default priority), so
    fullscreen is a synchronous borderless window, as on Windows. Confirmed in a user playtest (both directions,
    including "Yes" to keep the settings). Diagnose such issues with `NOITAMAC_TRACE=SDL_SetWindow,SDL_WINDOWEVENT,glViewport`
    (NOITAMAC_TRACE now takes comma-separated name filters; SDL_WINDOWEVENT logs events with window/drawable sizes).
    Scripted clicks now hover 100 ms before pressing (dialog buttons ignored un-hovered presses). New Game with a
    run in progress: `NOITAMAC_CLICKS="20:639,370;24:445,250;28:607,362"` (New Game, first mode, Yes).
  - **Determinism: matches Wine.** `tools/determinism.sh` installs `tools/seedprint` (WORLD_SEED=123456789; writes
    `seedprint.txt` with io, because release builds drop print() output even on Windows, so it needs the mod sandbox
    off, which install/remove toggle). The seed, ProceduralRandomf/Randomf, and the 512x512 cell grid plus all
    entities at spawn+60 and +600 frames are identical across one Wine and two noitamac runs. Spawn+1 differs
    even between noitamac runs (chunk streaming timing). Lua libm: 2 of 25 values differ in the last bit
    (sin(-2.5), exp(25.175)): the game's LuaJIT 2.0 uses x87 fsin/exp, ours uses the macOS libm. No visible effect.
    For a manual session: `caffeinate -u build/noitamac` (add `NOITAMAC_TRACE_LUA=1` for Lua loads and
    pcall errors; `longjmp` still exits 11, a sandboxed lib exits 10). Saves live under
    `~/Library/Application Support/noitamac/AppData/LocalLow/Nolla_Games_Noita/` (the loop enabled the
    `example` and `starting_loadouts` mods in save00/mod_config.xml and accepted the mod disclaimers in
    save_shared/config.xml).
  - Menu input (mouse and keyboard through SDL events).
  - Start a new game, world generation, moving and shooting, then save & quit and continue.
  - Windowed and fullscreen modes.
  - Determinism check: with the same seed, compare the starting area against the Wine build (screenshot
    or pixel data). Recompilation must not change world generation. Record any difference and its cause
    (for example host libm sin/cos/pow).
  - Record crashes and fixes.
- [ ] Performance: the point of the project.
  - Measure FPS and frame time for a fixed scene under simulation load, natively and under the Sikarugir
    Wine build.
  - Profile with Instruments and fix the top hotspots. Likely candidates are `guest_call` lookups, GPR
    syncs around calls, x87 helpers and bridge overhead.
  - Record the before/after numbers.
- [ ] Memory ordering audit.
  - Plain loads and stores have ARM64 ordering, not x86 TSO. With about 18 guest threads that can break
    lock-free code that relies on TSO.
  - Find the lock-free patterns in the exe (volatile flags, hand-rolled queues, double-checked init),
    decide where fences are needed, and add a lifter option such as emitting acquire/release for
    identified addresses or functions.
  - Add a stress test if feasible.
- [ ] Real audio. The FMOD Engine is downloaded: `~/Downloads/fmodstudioapi20123mac-installer.dmg`
  (2.01.23, the last 2.01.x; the game ships 2.01.05). Inside the volume "FMOD Programmers API Mac", the
  files are `FMOD Programmers API/api/{core,studio}/lib/libfmod{,studio}.dylib` (universal x86_64+arm64)
  and `api/{core,studio}/inc/`.
  - Add `tools/setup_fmod.sh` (like `setup_game.sh`): mount the dmg read-only, copy the dylibs and headers
    to `build/fmod/`, and check the `FMOD_VERSION` and that the dylibs contain arm64. Never commit these files.
  - Bridge the 42 functions to the native C++ or C API, with handles mapped between guest and host.
  - Callbacks go to guest code through trampolines, marshalled onto a thread that has a guest `CPU`.
  - Check that the game's `.bank` files load in the runtime version we get. If only a newer 2.x is
    available, note the compatibility result.
  - Keep `NOITAMAC_AUDIO=stub` working.
- [ ] Stubs for the remaining imports: WININET and WS2_32 fail cleanly (offline), `GetOpenFileNameA`
  returns cancel, `timeBeginPeriod`, `GetActiveWindow`, `CoTaskMemFree`, and the crash-handler thread
  APIs (`SuspendThread`/`GetThreadContext`) if they're reached. Check that nothing aborts in a
  30-minute session.
- [ ] Packaging.
  - Build a `Noita.app` that points to the user's own install.
  - Generated C is derived from the game and must not ship, so the app build runs the
    discover/lift/build pipeline locally against the user's exe (bring-your-own). Document that in the
    README.
- [ ] Manual playtest (needs a person; the loop should skip it): rebind a key in Options (keyboard and
  mouse), and play one 30+ minute session. Record crashes, hangs or anything wrong.
