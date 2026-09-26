# Runtime

## Guest memory map

The guest address space is one 4 GB host mapping (`MEM`, `rt_init`); guest address `a` is at `MEM + a`.

| Range | What | Defined in |
|---|---|---|
| `0x00000000`–`0x003FFFFF` | unused (NULL pointers land here) | |
| `0x00400000`–`0x012E6FFF` | `noita.exe`, at its preferred base | `tools/pe.py` `MODULES` |
| `0x08000000`–`0x1003FFFF` | difftest/test harness stack, code and scratch (tests only) | `tools/difftest.py` |
| `0x18000000`–`0x191FCFFF` | recompiled DLLs (msvcp120 `0x18000000` … SDL2 `0x19100000`); the launcher maps msvcp120 | `tools/pe.py` `MODULES`, `main.c` `DLLS` |
| `0x1A000000`–`0x1E3FFFFF` | thread slots n = 0..63, `0x110000` each: 64 KB `PROT_NONE` guard, then a 1 MB stack | `proc.h` |
| `0x1F000000`–`0x1F07FFFF` | thread slots n = 0..63, `0x2000` each: TEB page, then its TLS pointer array | `proc.h` |
| `0x1F800000` | PEB | `proc.h` |
| `0x1F810000` | process heap handle (a handle only; nothing is mapped there) | `proc.h` |
| `0x20000000`–`0xDFFFFFFF` | guest heap (3 GB) | `heap.h` |
| `0xF0000000`+ | host function thunks, 16 bytes apart (no code, looked up by `guest_call`); a data import (`_acmdln`, `_fmode`, `_commode`) is stored in its thunk's 16 bytes | `rt.h`, `msvcr120.c` |

Thread n (0 = main) has thread id `0x104 + 4n` in process `0x100`. Its TEB has `fs:[0]` = `0xFFFFFFFF`,
`fs:[4]`/`fs:[8]` = stack top/bottom, `fs:[0x18]` = itself, `fs:[0x2c]` = its TLS array (slot 0 = its copy of
the exe's static TLS block, allocated on the guest heap), `fs:[0x30]` = the PEB, `TlsAlloc` slots at `+0xE10`, and
the HLE CRT's errno at `+0xFF0`.

## Guest view of the host

The launcher's working directory is the game directory. The host file system is drive `Z:` (as in Wine), so
`GetCurrentDirectoryW` returns e.g. `Z:\Users\me\noitamac\build\game`; `host_path` (hle.c) maps relative and `Z:`
paths back. stdio does no text-mode CR/LF translation. Only the "C" locale exists.

## Files

| File | What |
|---|---|
| `cpu.h` | `CPU`, guest memory accessors and the x87/SSE/atomic helpers used by generated code |
| `main.c` | the launcher, `build/noitamac`: maps `noita.exe` and msvcp120.dll, binds imports, runs DllMain and the entry point |
| `rt.c`/`rt.h` | memory setup, PE mapping (`rt_map_pe`), function lookup, import binding (thunks, or exports of recompiled modules) and tracing, `guest_call` |
| `host.h` | `HOST_CDECL`/`HOST_STDCALL`/`HOST` declarations and argument/return helpers for host imports |
| `heap.c`/`heap.h` | the guest heap allocator |
| `proc.c`/`proc.h` | PEB, thread stacks, TEBs and static TLS |
| `msvcr120.c`, `kernel32.c` | native (HLE) implementations of those DLLs' imports; `msvcr120.h` has `crt_init` (command line, data imports) |
| `msvcr120_stdio.c` | FILE functions and the MSVC-style printf engine over guest varargs (`crt_vformat`) |
| `msvcr120_string.c`, `msvcr120_math.c`, `msvcr120_concrt.c` | mem/str/ctype/conversions/rand/locale; libm; ConcRT locks, events, condition variables |
| `shlwapi.c` | SHLWAPI (PathAppendW) |
| `sdl2_stdlib.c` | the SDL2 C-library helpers SDL2main's WinMain uses (SDL_malloc/free/wcslen/isspace/iconv_string) and SDL_SetMainReady, which traces SDL_main's entry |
| `hle.c`/`hle.h` | helpers shared by HLE files: guest strings, UTF-8/16, Windows paths, errno |
| `sync.c`/`sync.h` | pool of host mutex/condvar objects behind guest critical sections and ConcRT objects |
| `undname.c`/`undname.h` | MSVC RTTI type name undecoration for `type_info::name` (checked by `tools/undnametest.py`) |
| `*_test.c`, `harness.c`, `bench.c` | tests and benchmarks, run by `tools/check.sh` |
