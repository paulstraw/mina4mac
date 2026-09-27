// Shared runtime: guest memory setup, image loading and guest function lookup.
// Programs linking rt.c provide FN_TABLE/FN_COUNT (generated, sorted by address).
#pragma once
#include "cpu.h"

typedef struct { uint32_t addr; GuestFn fn; } FnEntry;
extern const FnEntry FN_TABLE[];
extern const int FN_COUNT;

// Map the 4 GB guest address space at MEM. Exits on failure.
void rt_init(void);

// Copy a flat memory image file to guest address `base`. Exits on failure.
void rt_load_image(const char *path, uint32_t base);

// Map a PE32 file into guest memory at `base`, as the Windows loader would: headers and each section at
// its RVA (zero fill beyond raw data comes from the zeroed mapping), with base relocations applied if
// `base` differs from the preferred ImageBase. Returns the guest address of the entry point. Exits on
// failure. Imports are not bound (see rt_bind_imports).
uint32_t rt_map_pe(const char *path, uint32_t base);

// Recompiled function starting at guest address `addr`, or NULL. O(1): a two-level table indexed by
// addr >> 12, whose second-level pages are built from FN_TABLE on first use.
GuestFn rt_lookup(uint32_t addr);

// Host functions (imports). Thunk n is the guest address THUNK_BASE + THUNK_STRIDE * n; it holds no code,
// guest_call on it runs the host implementation registered for its dll!name. A host implementation is
// entered like the guest function it replaces: the return address is at [esp], and it must pop it (plus
// any stdcall arguments), as the `ret` would. Calling a thunk without an implementation exits with code 4,
// printing dll!name and the guest return address.
#define THUNK_BASE 0xF0000000u
#define THUNK_STRIDE 16u

// Nonzero: log every host import call to stderr (dll!name, return address, the first stack arguments,
// then eax/edx and the bytes popped on return). Set by the launcher from MINA4MAC_TRACE.
extern int rt_trace;
// NULL: trace every import; otherwise only those whose "dll!name" contains one of these comma-separated
// substrings (case-insensitive). Set by the launcher from MINA4MAC_TRACE when it isn't "1".
extern const char *rt_trace_filter;
int rt_traced(const char *dll, const char *name);  // rt_trace, and dll!name passes rt_trace_filter

// Count calls per import thunk (including GetProcAddress thunks) and write "calls<TAB>dll!name" for every
// thunk to `path` at exit. Set by the launcher from MINA4MAC_COUNT_IMPORTS.
void rt_count_imports(const char *path);

// Register the host implementation of dll!name (dll matched case-insensitively). May be called before
// or after binding.
void rt_register_import(const char *dll, const char *name, GuestFn fn);

// A host implementation of dll!name is registered.
int rt_has_import(const char *dll, const char *name);

// Thunk address for dll!name (ordinal imports are named "#<n>"), created on first request.
uint32_t rt_thunk(const char *dll, const char *name);

// Declare that dll (matched case-insensitively) is a recompiled guest module mapped at guest `base`
// (headers included). rt_bind_imports then binds imports from it to its exports instead of thunks.
void rt_register_module(const char *dll, uint32_t base);

// Guest base of registered module dll (matched case-insensitively), or 0.
uint32_t rt_module_base(const char *dll);

// Guest address of export `name` ("#<n>" for an ordinal) of the PE image mapped at `base`, or 0.
uint32_t rt_export(uint32_t base, const char *name);

// Bind every IAT slot of the PE image mapped at guest `base` (headers included): to the export's address
// if its dll is a registered module (exits if the export is missing), otherwise to a host thunk. Returns
// the number of slots bound.
int rt_bind_imports(uint32_t base);
