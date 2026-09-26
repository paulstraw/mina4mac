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

// Register the host implementation of dll!name (dll matched case-insensitively). May be called before
// or after binding.
void rt_register_import(const char *dll, const char *name, GuestFn fn);

// Thunk address for dll!name (ordinal imports are named "#<n>"), created on first request.
uint32_t rt_thunk(const char *dll, const char *name);

// Write a thunk address into every IAT slot of the PE image mapped at guest `base` (headers included).
// Returns the number of slots bound.
int rt_bind_imports(uint32_t base);
