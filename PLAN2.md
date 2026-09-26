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
- [ ] Run to the first `lua51` call, then inventory how the game uses Lua, to feed Phase 5.
  - Which of the 171 imports are called and which are only address-taken? Log the address-taken ones
    (the sandbox patches) together with the code that patches them.
  - How many `lua_State`s are created, on which threads, and with which `luaopen_*` libraries?
  - Which lua functions return floats in st0, and which take or return structs (`lua_Debug`)?
  - Does anything read LuaJIT object internals directly (for example `L->top`) instead of going through
    the API?
  - Record the answers in the note. Do not implement Lua yet.

### Phase 5: LuaJIT

- [ ] Host LuaJIT bridge (`runtime/lua51.c`).
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
- [ ] Run until the game's Lua init finishes (built-in scripts and mod loading with the default mods).
  Record which scripts ran, any API gaps, and where execution stops.

### Phase 6: OpenGL

- [ ] GL survey, before writing the bridge.
  - Count at runtime every GL name the game actually calls (the thunk path), not just the 1,037 it looks
    up.
  - Extract the shaders from `data.wak` (document the wak format in the note) and record their GLSL
    `#version`s and any fixed-function use.
  - Decide between the legacy 2.1 and the core 4.1 context. Record the list of names that macOS lacks.
- [ ] Generate the GL bridge (`tools/gen_gl.py` from `gl.xml`, output under `build/gen_all/`) for the
  names the survey found, all `__stdcall`.
  - Pointer arguments are guest pointers (`MEM + p`). But `gl*Pointer`, `glDrawElements` indices and
    `glTex*Image` with a bound pixel-unpack buffer take buffer *offsets* when a buffer is bound, and those
    must not be translated.
  - `glGetString` copies into guest memory.
  - `glMapBuffer*` returns a guest shadow buffer that's copied back on unmap.
  - GetProcAddress returns NULL for names macOS doesn't provide, as a real driver would.
  - Replace the no-context workaround in `opengl32.c`.
  - Add a gltest step with a hidden window: clear, draw a triangle and `glReadPixels` through the thunks.
- [ ] First frame. Run until the main menu renders. Save `build/menu.png` with `tools/screenshot.sh` and
  fix blockers along the way (including C++ exceptions or `longjmp` if they're hit). Record the blockers.

### Phase 7: playable

- [ ] Play a run.
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
- [ ] Real audio (**needs the user to download the FMOD Studio API 2.01.x for macOS** from fmod.com).
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
