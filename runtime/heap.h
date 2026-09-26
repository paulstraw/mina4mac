// The guest heap: a thread-safe allocator over the fixed guest range [HEAP_LO, HEAP_HI). It backs the
// CRT (malloc/free/realloc/calloc, operator new/delete, _aligned_malloc) and KERNEL32's Heap* functions.
// All addresses are guest addresses; 0 means failure / NULL. Blocks are 16-byte aligned, with a 16-byte
// header in guest memory just below the pointer.
#pragma once
#include <stdint.h>

#define HEAP_LO 0x20000000u
#define HEAP_HI 0xE0000000u

uint32_t heap_alloc(uint32_t size);                    // uninitialised; size 0 gives a unique pointer
uint32_t heap_calloc(uint32_t n, uint32_t size);       // zeroed; 0 on n*size overflow
uint32_t heap_realloc(uint32_t p, uint32_t size);      // p = 0 allocates; size 0 still returns a block
uint32_t heap_resize(uint32_t p, uint32_t size);       // in place only: p, or 0 if it doesn't fit
void heap_free(uint32_t p);                            // p = 0 is a no-op
uint32_t heap_size(uint32_t p);                        // the size last requested for p
int heap_owns(uint32_t p);                             // p is a live block from this heap

// Aligned blocks, laid out like MSVC's _aligned_malloc (the original pointer is stored just below the
// aligned one), so they must be freed with heap_aligned_free. align must be a power of two.
uint32_t heap_aligned_alloc(uint32_t size, uint32_t align);
void heap_aligned_free(uint32_t p);
