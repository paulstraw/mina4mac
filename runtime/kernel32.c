// KERNEL32 implemented natively (HLE). Every heap handle maps to the one guest heap (heap.c).
#include <string.h>

#include "heap.h"
#include "host.h"
#include "proc.h"

enum { HEAP_ZERO_MEMORY = 0x8, HEAP_REALLOC_IN_PLACE_ONLY = 0x10 };

HOST_STDCALL(kernel32, GetProcessHeap, 0) { ret_i32(c, PROCESS_HEAP); }

HOST_STDCALL(kernel32, HeapAlloc, 12) {  // (heap, flags, bytes)
    ret_i32(c, ARG(1) & HEAP_ZERO_MEMORY ? heap_calloc(1, ARG(2)) : heap_alloc(ARG(2)));
}

HOST_STDCALL(kernel32, HeapFree, 12) {  // (heap, flags, mem)
    heap_free(ARG(2));
    ret_i32(c, 1);
}

HOST_STDCALL(kernel32, HeapReAlloc, 16) {  // (heap, flags, mem, bytes)
    uint32_t flags = ARG(1), p = ARG(2), size = ARG(3);
    if (!p) return ret_i32(c, 0);
    uint32_t old = heap_size(p);
    uint32_t q = flags & HEAP_REALLOC_IN_PLACE_ONLY ? heap_resize(p, size) : heap_realloc(p, size);
    if (q && (flags & HEAP_ZERO_MEMORY) && size > old) memset(P(q + old), 0, size - old);
    ret_i32(c, q);
}

HOST_STDCALL(kernel32, HeapSize, 12) { ret_i32(c, heap_size(ARG(2))); }  // (heap, flags, mem)
