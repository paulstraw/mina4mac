# Runtime

## Guest memory map

The guest address space is one 4 GB host mapping (`MEM`, `rt_init`); guest address `a` is at `MEM + a`.

| Range | What | Defined in |
|---|---|---|
| `0x00000000`–`0x003FFFFF` | unused (NULL pointers land here) | |
| `0x00400000`–`0x012E6FFF` | `noita.exe`, at its preferred base | `tools/pe.py` `MODULES` |
| `0x08000000`–`0x1003FFFF` | difftest/test harness stack, code and scratch (tests only) | `tools/difftest.py` |
| `0x18000000`–`0x191FCFFF` | recompiled DLLs (msvcp120 `0x18000000` … SDL2 `0x19100000`) | `tools/pe.py` `MODULES` |
| `0x1A000000`–`0x1E3FFFFF` | thread slots n = 0..63, `0x110000` each: 64 KB `PROT_NONE` guard, then a 1 MB stack | `proc.h` |
| `0x1F000000`–`0x1F07FFFF` | thread slots n = 0..63, `0x2000` each: TEB page, then its TLS pointer array | `proc.h` |
| `0x1F800000` | PEB | `proc.h` |
| `0x1F810000` | process heap handle (a handle only; nothing is mapped there) | `proc.h` |
| `0x20000000`–`0xDFFFFFFF` | guest heap (3 GB) | `heap.h` |
| `0xF0000000`+ | host function thunks, 16 bytes apart (no code, looked up by `guest_call`) | `rt.h` |

Thread n (0 = main) has thread id `0x104 + 4n` in process `0x100`. Its TEB has `fs:[0]` = `0xFFFFFFFF`,
`fs:[4]`/`fs:[8]` = stack top/bottom, `fs:[0x18]` = itself, `fs:[0x2c]` = its TLS array (slot 0 = its copy of
the exe's static TLS block, allocated on the guest heap), `fs:[0x30]` = the PEB, and `TlsAlloc` slots at `+0xE10`.

## Files

| File | What |
|---|---|
| `cpu.h` | `CPU`, guest memory accessors and the x87/SSE/atomic helpers used by generated code |
| `main.c` | the launcher, `build/noitamac`: maps `noita.exe`, sets up the process and runs the entry point |
| `rt.c`/`rt.h` | memory setup, PE mapping (`rt_map_pe`), function lookup, import thunks and tracing, `guest_call` |
| `host.h` | `HOST_CDECL`/`HOST_STDCALL`/`HOST` declarations and argument/return helpers for host imports |
| `heap.c`/`heap.h` | the guest heap allocator |
| `proc.c`/`proc.h` | PEB, thread stacks, TEBs and static TLS |
| `msvcr120.c`, `kernel32.c` | native (HLE) implementations of those DLLs' imports |
| `*_test.c`, `harness.c`, `bench.c` | tests and benchmarks, run by `tools/check.sh` |
