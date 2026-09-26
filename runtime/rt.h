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
