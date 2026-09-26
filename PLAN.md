# noitamac: native Noita on Apple Silicon via static recompilation

## Context

### Goal

Run Noita (a Windows-only, 32-bit x86 game) natively on an M1 Max Mac without Wine or Rosetta. We
statically recompile `noita.exe` (x86-32 machine code) to C, compile that C for ARM64, and supply a
native runtime that stands in for Windows and the game's DLLs.

Wine was tried first and rejected. Even Wine 11 (Sikarugir) drops frames under light simulation load,
because Wine+Rosetta thread synchronisation overhead dominates. So Wine is not an option for this project.

### Legal and repository rules (hard constraints)

- This is a personal-use, bring-your-own-exe tool. **Never commit game files**: no `.exe`, `.dll`,
  extracted data, `image.bin`, or generated C (which is derived from the game).
- Everything under `build/` is gitignored. Keep it that way.
- The game binaries live in `build/game/`, a gitignored copy of the user's GOG install:
  `noita.exe`, `noita_dev.exe`, `msvcr120.dll`, `msvcp120.dll`, `SDL2.dll`, `lua51.dll`, `fmod.dll`,
  `fmodstudio.dll`, `Galaxy.dll`.
  - The copy was taken from `~/Applications/Noita Sikarugir.app/Contents/SharedSupport/prefix/drive_c/GOG Games/Noita`.
    That folder also holds `data/`, `mods/` and the other game files, which will be needed later.
  - `NOITA_DIR` overrides the location.
- Commit at the end of every task with a conventional commit message (`feat:`, `fix:`, `refactor:`, `test:`, `chore:`).
  Only commit source, tools and docs.

### Toolchain

- Python via `uv`. Run tools as `uv run tools/<name>.py`. Dependencies are capstone, unicorn and pefile.
- Apple clang. Homebrew is at `/opt/homebrew`. Install `sdl2` etc. with `brew` when a task needs it.

### Repository layout

| Path | What it is |
|---|---|
| `tools/pe.py` | PE loader (`load()` → `Image`), with sections, relocations (`relocs`) and imports (IAT slot VA → `(dll, name)`). |
| `tools/discover.py` | Recursive-descent code discovery over `noita.exe`. Seeds come from the entry point, relocated pointers in data sections, and relocated immediates in code. Jump tables are resolved via relocations. Writes `build/discover.pkl` (instructions, function starts, jump tables). Takes about 30 s. Re-run only if discovery logic changes. |
| `tools/lift.py` | The recompiler. `Program` holds whole-image knowledge; `FnLifter(prog, addr).lift()` returns C for one function. |
| `tools/survey.py` | Lifts every function and reports the success rate plus the top blockers. Writes `build/survey.pkl`. Takes about 50 s. |
| `tools/build_all.py` | Lifts all functions into 128 chunk files in `build/gen_all/`, compiles them in parallel and links `build/harness_all`. Takes about 2.5 min. Unliftable functions become stubs that call `guest_unimpl`. |
| `tools/difftest.py` | Differential tester: Unicorn (reference x86) vs recompiled native code, run on identical random inputs. |
| `runtime/cpu.h` | `CPU` struct, the guest memory model, and the x87/SSE helpers shared by all generated code. |
| `runtime/harness.c` | Difftest harness. Loads `build/image.bin` at 0x400000, applies a snapshot, runs one function, dumps state. |
| `runtime/bench.c` | Microbenchmark: recompiled RNG (0xdda6b0) vs a hand-written equivalent, plus the noise function 0xc3ef20. |

### How the recompiled code works

- **Guest memory.** Guest memory is one 4 GB `mmap` (`MAP_NORESERVE`), and guest address `a` lives at `MEM + a`.
  `noita.exe` is mapped at its preferred base, 0x400000. The game is 32-bit, so every pointer it stores is a
  32-bit guest address.
- **Functions.** Each guest function becomes `void F_<8-hex-addr>(CPU *restrict c)`.
  - Guest GPRs and the arithmetic flags are C locals inside the function body. They're loaded from `c` at
    entry, and GPRs are synced to `c` around every call and at return.
  - Flags are never live across calls (compiler-generated code), so clang removes unused flag computations.
  - Xmm and x87 state live in `c`. x87 is modelled in double precision: MSVC's default precision control is
    53-bit and the `fpu_cw` field holds the rounding mode.
- **Guest stack.** The guest stack is real guest memory. A `call` pushes the real return address, so stack
  layouts match the original exactly. `ret n` pops 4+n and returns from the C function. Return addresses are
  never used for control flow.
- **Calls.**
  - Direct calls go to `F_xxx(c)`.
  - Indirect calls and tail calls go through `guest_call(c, target)`, which looks up a sorted
    `FN_TABLE[]` with binary search.
  - Calls through import slots go to `guest_import(c, slot)`, which the harness currently stubs to abort.
  - A `jmp` to any known function start is a tail call. Switches become C `switch` statements over
    jump-table targets.
- **fs segment.** `fs:` accesses become `c->fs_base + offset`. About 10k functions install C++ EH frames
  via `fs:[0]`; that's just memory traffic and needs no special handling. Only one real `_CxxThrowException`
  call site exists.
- **Build flags.** Compile with `-O2 -ffp-contract=off -fno-strict-aliasing`. Do not allow FMA contraction,
  because float results must match x86 bit-for-bit (world generation and seeds depend on it).

### Current status (end of the feasibility spike)

- **Code discovery:** 99.9% of `.text` is accounted for. There are 97,087 function starts and 821 jump
  tables, and 457 indirect `jmp`s that are vtable tail calls.
- **Lifting:** 97,059 of 97,087 functions (99.97%). The 28 left use psllq/psrlq, rcr, imul r/m8/16, some
  punpck*/paddb/pmovsxbd and cpuid, and a few are misdecoded data (`aas`, segment moves).
- **Differential testing:** 0 mismatches over ~4.6k comparable trials, across ~3.5k random functions and
  x87-heavy code.
- **Performance:** the recompiled RNG runs at native speed (12.7 ns vs 13.6 ns hand-written).

### Facts about the binary that later work depends on

- Built with VS2013 (VC12) as a unity build. The PDB path is `falling_everything.pdb`. There are 2,484 RTTI
  classes.
- **Import surface, as functions called:**

  | Module | Functions |
  |---|---|
  | lua51 (LuaJIT 2.0.4) | 171 |
  | MSVCP120 | 141 |
  | MSVCR120 | 131 |
  | SDL2 | 66 |
  | KERNEL32 | 52 |
  | fmodstudio | 35 |
  | fmod | 7 |
  | WININET | 6 |
  | Galaxy | 6 |
  | SHELL32 | 3 |
  | USER32, WINMM, SHLWAPI, ole32, COMDLG32 | 1 each |

- **Module preferred bases.** Every DLL has relocations, so any of them can be rebased.

  | Module | Base | Size |
  |---|---|---|
  | noita.exe | 0x400000 | 0xee7000 |
  | SDL2 | 0x6c740000 | 0xfd000 |
  | msvcr120 | 0x10000000 | 0xee000 |
  | msvcp120 | 0x10000000 | 0x71000 |
  | lua51 | 0x10000000 | 0x5a000 |
  | fmod | 0x10000000 | 0x1bc000 |
  | fmodstudio | 0x10000000 | 0x114000 |
  | Galaxy | 0x10000000 | 0x9c7000 |

- **OpenGL.** The game loads `opengl32.dll` itself (LoadLibrary + GetProcAddress, with a
  "Failed loading win32 opengl32.dll!" message), and its GL loader lists 1,037 GL names. GL entry points
  are `__stdcall` on Win32. Plan: return thunk addresses from GetProcAddress and bridge to macOS OpenGL.
  macOS's compatibility profile is GL 2.1, which is probably enough for Noita's renderer, but that's
  unverified.
- **Less common runtime features:**
  - `longjmp` is used at 46 call sites and `_setjmp3` at 1, probably libpng/libjpeg error paths.
  - `GetThreadContext`, `SetThreadContext` and `SuspendThread` each have one call site, probably a
    watchdog or crash handler.
  - The exe has `.detourd`/`.detourc` sections (Microsoft Detours). Check that nothing patches `.text` at
    runtime.
- **Threads.** The game runs about 17 worker threads for the simulation. Guest threads need real host
  threads, each with its own guest stack and TEB. Lock-prefixed instructions are currently lifted as
  non-atomic.
- **Later phases, not tasks yet:**
  - The OpenGL bridge (generate it from the Khronos `gl.xml`).
  - LuaJIT: native ARM64 LuaJIT 2.1 with its allocator placed inside guest memory, so the pointers it
    returns are 32-bit guest addresses. Mods that use FFI on game memory will likely break.
  - FMOD: Mac libraries for the matching FMOD Studio version.
  - Galaxy and WININET stubs, threads, real C++ exceptions if they're ever needed, and packaging as a `.app`.

### Verification (run after any change to the lifter or runtime)

1. `uv run tools/survey.py`. The lifted count must not go down.
2. `uv run tools/build_all.py`. It must compile cleanly.
3. `uv run tools/difftest.py --all --funcs 1500 --trials 3 --seed <any>`, then the same command with `--x87`.
   Both must report `'fail': 0, 'native_err': 0`.
   - `ref_skip` counts random inputs that crashed Unicorn. It's expected and harmless.
   - `--only <hex>` retests a single function.

Gotchas that already bit us:
- An x86 string op is only a string op if its operands say `es:[edi]`. `movsd` is also an SSE
  instruction.
- Emitted bodies are sorted by address, so emit `goto L_<start>` when the entry point isn't the lowest
  address.
- Unicorn needs a GDT with flat CS/DS/SS/ES plus an FS descriptor based at the TEB. Reset its FPU
  (FPSW/FPTAG/FP0-7, FPCW=0x27F) every trial.
- The native harness must load the whole image (`build/image.bin`, written by difftest), not just `.data`.

### Working rules for each task

- Read this whole file and the notes under completed tasks before starting.
- Keep the generated-code conventions consistent (`F_%08x`, `CPU *restrict c`, the helpers in `cpu.h`).
- Add tests or verification output that prove the task works, and state the evidence in your task note.
- If a task is clearly too big for one session, do not check it. Explain why and suggest how to split it.

## Tasks

### Phase 0: finish the recompiler core

- [x] Add `tools/check.sh`, which runs the full verification sequence above (survey, build_all, difftest
  default and `--x87`). It prints a one-line summary per step and exits non-zero if the lifted count drops
  or any difftest `fail`/`native_err` is non-zero. Run it once and record the numbers.
  - `tools/check.sh [seed]` (~6 min; logs in `build/check/`); the baseline is in `tools/lifted_baseline.txt` and rises automatically. Seed 18671: lifted 97059/97087, 28 stubbed; default pass 2250/fail 0/native_err 0; x87 pass 152/fail 0/native_err 0.
  - Fixed a false native_err in difftest: stack, scratch and TEB are now mapped non-exec in Unicorn, so indirect calls into random data count as ref_skip.
- [x] Lift the remaining 28 functions. Implement psllq/psrlq/punpck*/paddb/pmovsxbd/rcr/imul r/m8+16, and
  cpuid returning fixed values for an SSE2-capable Intel CPU. Leave genuine misdecodes as stubs and list
  their addresses in the note. Verify with `tools/check.sh`, plus `difftest --only` on each newly lifted
  function.
  - 97087/97087 lifted, 0 stubs (check.sh seed 4242: default 2255 pass, x87 159 pass, 0 fail/native_err). No real misdecodes: 0x874fd0 ran into a jump table after a noreturn `_Xlength_error`, so calls to `NORETURN_IMPORTS` (lift.py) now end a block. Also added more SSE int ops, cmpXXsd, pushfd, segment-reg reads (WoW64 selectors), and cmpxchg8b (not atomic yet). Fixed `orps` being lifted as AND, and the harness now starts with fpu_cw=0x27f.
  - New `tools/insntest.py` runs single-instruction Unicorn-vs-native tests on real instances, plus `--at <addrs>`. All new ops pass (6607 trials). cpuid values are in `cpuid_fixed` (cpu.h), and difftest hooks Unicorn with the same values. imul r/m16 never occurs in the binary, so it's untested.
- [x] Give lock-prefixed instructions and `xchg` with a memory operand real atomic semantics, using clang
  `__atomic` builtins on `MEM + addr`: lock xadd/cmpxchg/cmpxchg8b/add/inc/dec/or/and. Verify with
  `tools/check.sh`, and add a native unit test that hammers `lock xadd` from several host threads through
  recompiled code.
  - `FnLifter.lift_locked` emits seq_cst `lk_{xchg,cas,add,sub,and,or,xor}{8,16,32,64}` helpers (cpu.h). Misaligned addresses fall back to non-atomic, because ARM64 atomics SIGBUS. The binary only uses lock xadd (72), xchg mem (79), lock cmpxchg (2) and lock cmpxchg8b (2); the other lock ops are untested. Plain loads/stores still lack x86 TSO ordering.
  - `tools/atomictest.py` + `runtime/atomic_test.c`: 8 threads × 200k on real lifted xadd/xchg-spinlock/cmpxchg/cmpxchg8b, all exact (the old lifter hangs). Added to check.sh. check.sh seed 2601: 97087 lifted; default 2254 / x87 156 pass, 0 fail/native_err. insntest: 1288 pass. difftest now counts "no function at" as ref_skip.
- [x] Move shared runtime code (MEM setup, image loading, function lookup, `guest_*`) from `harness.c` and
  `bench.c` into `runtime/rt.c` + `runtime/rt.h`. Replace the binary-search lookup in `guest_call` with an
  O(1) two-level page table (guest addr >> 12 → page of `GuestFn`s). Verify with `tools/check.sh` and
  check that `build/bench` shows no regression.
  - `rt.h` has `FnEntry`/`FN_TABLE`, `rt_init`, `rt_load_image` and `rt_lookup`. Pages are built lazily from FN_TABLE and CAS-published (all 2,565 eagerly would be ~80 MB); empty pages share one zero page. Every harness (difftest, insntest, atomictest) now links rt.c, and build_all also links `build/bench`.
  - check.sh seed 7331: 97087 lifted; default 2248 / x87 153 pass, 0 fail/native_err; atomictest ok. Bench is unchanged (rng 12.4 ns, noise 123 ns), and lookup takes 2.1 ns vs 60 ns for the binary search.

### Phase 1: multiple modules

- [x] Generalise `pe.py`/`discover.py`/`lift.py`/`build_all.py` from "noita.exe" to a module list. Each
  module has a file, a chosen load base (non-overlapping, recorded in one place) and relocations applied
  when the base differs from its preferred one. Module-specific outputs go under `build/<module>/`.
  noita.exe results must be unchanged: the same survey numbers and a clean `tools/check.sh`.
  - `pe.MODULES` holds name → (file, base): noita 0x400000, DLLs packed from 0x18000000 (msvcp120 0x18000000, …, SDL2 0x19100000), clear of heap/thunks/difftest areas. `load(module)` rebases via relocs; `Image.exports` added and discover seeds exports. `discover.py`/`survey.py [module]`, `Program(module)`, `build_all.py [module ...]` (chunks in `build/<m>/gen/`, combined decls/FN_TABLE in `build/gen_all/`). pkl files, image.bin and test scratch C (`testgen/`) now live in `build/noita/`.
  - noita discover.pkl is byte-identical to before. check.sh seed 5150: 97087/97087 lifted, 0 stubs; default 2247 / x87 157 pass, 0 fail/native_err; atomictest ok; insntest 6830 pass; bench unchanged (12.7 ns rng).
- [x] Discover and survey `msvcp120.dll` at a chosen base. Record the lift coverage and the list of what
  it imports (dll!name counts) in the note.
  - Decision rule: if at least 99.5% of its functions lift, it is recompiled like the exe (the plan
    assumes this).
  - Otherwise note that it must be reimplemented natively.
  - msvcr120 and KERNEL32 are always implemented natively in C (HLE).
  - **Recompile it.** At 0x18000000, 3,033/3,042 functions lift (99.7%). The 3 real misses are `lock bts`, `lock btr` and `cbw` (0x180126cb/d8, 0x18027770); the other 6 are false seeds in data. msvcp120 merges .rdata into .text, so discover.py now requires pointer/export seeds to decode as plausible code (`plausible`, `JUNK`) and seeds vtables that sit in .text. noita's discover.pkl is byte-identical.
  - Imports: MSVCR120 173 names / 805 call sites (heavy: free 30, ??3 64, ConcRT PPL locks/events, exception ctors, sprintf_s 24, localeconv 18, `_errno` 19, stdio f* and wide file ops); KERNEL32 32 names / 77 sites (CriticalSection, Encode/DecodePointer, File*W, MultiByte/WideChar, GetLastError 12). 1,200 exports. Full per-name counts: `import_calls` in `build/msvcp120/discover.pkl`.

### Phase 2: runtime and startup

- [x] Import thunks.
  - Reserve a guest address range for host functions, e.g. 0xF0000000 + 16*n.
  - The loader writes a thunk address into every IAT slot (exe and recompiled DLLs). Change the lifter so
    `call [IAT slot]` and `jmp [IAT slot]` become an ordinary `guest_call(c, rd32(slot))`, and remove
    `guest_import`.
  - `guest_call` on a thunk address invokes the host implementation.
  - Calling an unimplemented import aborts, printing `dll!name` and the guest return address.
  - Verify with `tools/check.sh` and a unit test that calls a fake host import through a thunk.
  - rt.h: `THUNK_BASE`/`THUNK_STRIDE`, `rt_bind_imports(base)` (parses the PE import dir in guest memory), `rt_thunk(dll,name)`, `rt_register_import(dll,name,GuestFn)`; host fns pop their own ret/args; unimplemented exits 4. Thunks are only checked on rt_lookup miss (bench unchanged). All slots bind to thunks, incl. MSVCP120 — resolving to recompiled msvcp120 exports is still TODO. `FnLifter.import_slots` replaces survey's guest_import grep.
  - `tools/importtest.py` + `runtime/import_test.c` (in check.sh): binds 635 noita slots, lifted call/jmp [slot] + direct guest_call, unimpl message; 15 checks ok. check.sh seed 3141: 97087 lifted; default 2286 / x87 156 pass, 0 fail/native_err.
- [ ] Host function registry.
  - Add a C table or macros to declare host implementations by `dll!name`, with a calling convention
    (cdecl, or stdcall with arg bytes).
  - Add helpers for reading args from the guest stack (`ARG(n)`, `ARG_PTR(n)` → host pointer,
    `ARG_STR(n)`, `ARG_F32/F64`).
  - Handle returns in eax, eax:edx, x87 st0 (float returns) and xmm0, and correct stack cleanup per
    convention.
  - Unit-test cdecl and stdcall paths.
- [ ] Guest process environment.
  - A guest heap: a thread-safe allocator over a fixed guest range such as 0x20000000–0xE0000000, backing
    malloc/free/realloc/calloc/_aligned_malloc and HeapAlloc.
  - A 1 MB main-thread guest stack.
  - A TEB/PEB with fs:[0]=0xFFFFFFFF SEH chain, fs:[4]/[8] stack base/limit, fs:[0x18] self, fs:[0x2c]
    TLS array, fs:[0x30] PEB and thread id.
  - Record the memory map in `runtime/README.md`.
- [ ] Add a `build/noitamac` launcher (`runtime/main.c`, linked with `build/gen_all` objects).
  - It maps `noita.exe` sections from `build/game/noita.exe` itself (not `image.bin`), sets up the process
    and calls the entry point 0xdfadb0.
  - `NOITAMAC_TRACE=1` logs every host import call (name, args, return).
  - Expected result: it aborts at the first unimplemented import. Record the trace in the note.
- [ ] Implement KERNEL32/MSVCR120 imports until CRT startup reaches `_initterm`, including
  `__security_init_cookie`'s time/pid/tid/counter calls and `__set_app_type`, `_controlfp_s` and friends.
  `_initterm` calls guest function pointers via `guest_call`. Record the imports implemented and where
  execution stops.
- [ ] Run until all C++ static initializers (the `_initterm` tables) complete. Implement the msvcr120
  imports they hit, and recompile or implement the msvcp120 ones according to the Phase 1 decision. Record
  which initializers were problematic.
- [ ] Run until the program's main entry (WinMain → SDL's `SDL_main`) is entered. Identify its address,
  note it, and log entry with the trace.

### Phase 3: first window

- [ ] SDL bridge.
  - `brew install sdl2`.
  - Write `tools/gen_sdl.py` to generate host thunks for the 66 imported SDL2 functions from the SDL2
    headers, with marshalling for guest pointers, strings and structs whose 32-bit layout differs from
    64-bit (for example `SDL_Event` members holding pointers).
  - Unit-test `SDL_GetVersion` and `SDL_GetTicks` through the thunks.
- [ ] Run the game until `SDL_Init` and `SDL_CreateWindow` succeed and a window appears on screen.
  - Save a screenshot with `screencapture -l` into `build/`.
  - Record how far execution got and the next blocker (probably the opengl32 LoadLibrary/GetProcAddress
    path).
