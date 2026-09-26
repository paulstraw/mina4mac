// MSVCR120 implemented natively (HLE). Memory allocation is backed by the guest heap (heap.c).
#include <stdio.h>
#include <stdlib.h>

#include "heap.h"
#include "host.h"

HOST_CDECL(msvcr120, malloc) { ret_i32(c, heap_alloc(ARG(0))); }
HOST_CDECL(msvcr120, _malloc_crt) { ret_i32(c, heap_alloc(ARG(0))); }
HOST_CDECL(msvcr120, calloc) { ret_i32(c, heap_calloc(ARG(0), ARG(1))); }
HOST_CDECL(msvcr120, _calloc_crt) { ret_i32(c, heap_calloc(ARG(0), ARG(1))); }
HOST_CDECL(msvcr120, free) { heap_free(ARG(0)); }
HOST_CDECL(msvcr120, _msize) { ret_i32(c, heap_size(ARG(0))); }

static uint32_t crt_realloc(uint32_t p, uint32_t size) {  // realloc(p, 0) frees p and returns NULL
    if (p && !size) { heap_free(p); return 0; }
    return heap_realloc(p, size);
}
HOST_CDECL(msvcr120, realloc) { ret_i32(c, crt_realloc(ARG(0), ARG(1))); }
HOST_CDECL(msvcr120, _realloc_crt) { ret_i32(c, crt_realloc(ARG(0), ARG(1))); }

HOST_CDECL(msvcr120, _aligned_malloc) { ret_i32(c, heap_aligned_alloc(ARG(0), ARG(1))); }
HOST_CDECL(msvcr120, _aligned_free) { heap_aligned_free(ARG(0)); }

// operator new would throw std::bad_alloc; with no guest exceptions yet, running out is fatal.
static uint32_t op_new(uint32_t size) {
    uint32_t p = heap_alloc(size);
    if (!p) { fprintf(stderr, "operator new(%u): out of guest heap\n", size); exit(6); }
    return p;
}
HOST(msvcr120, op_new, "??2@YAPAXI@Z", 0) { ret_i32(c, op_new(ARG(0))); }
HOST(msvcr120, op_delete, "??3@YAXPAX@Z", 0) { heap_free(ARG(0)); }
HOST(msvcr120, concrt_alloc, "?Alloc@Concurrency@@YAPAXI@Z", 0) { ret_i32(c, op_new(ARG(0))); }
HOST(msvcr120, concrt_free, "?Free@Concurrency@@YAXPAX@Z", 0) { heap_free(ARG(0)); }
