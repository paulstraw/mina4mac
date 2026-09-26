// Guest heap. Small blocks (up to SMALL_MAX bytes with header) come from size classes, four per
// doubling, each carved from chunks and recycled through a per-class free list threaded through the
// freed blocks. Larger blocks are whole guest pages, taken first-fit from a sorted list of free spans
// (coalesced on free) or from a bump pointer `top` that grows up from HEAP_LO. One lock guards it all.
//
// Block header (16 bytes, guest memory, just below the returned pointer):
//   +0 requested size   +4 class index, or LARGE   +8 block bytes incl. header   +12 LIVE or DEAD
#include "heap.h"

#include <os/lock.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "cpu.h"

enum {
    HDR = 16,
    PAGE = 4096,           // guest page: large blocks are rounded to this
    SMALL_MAX = 32768,     // largest small block, header included
    LARGE = 0xffff,
    LIVE = 0x48454150,     // "PAEH"
    DEAD = 0x45455246,     // "FREE"
    MAX_CLASSES = 64,
    MAX_SPANS = 1 << 16,
};

static os_unfair_lock LOCK = OS_UNFAIR_LOCK_INIT;
static uint32_t CLASS_SIZE[MAX_CLASSES], NCLASSES;
static uint32_t FREE_LIST[MAX_CLASSES];               // head block address per class, 0 = empty
static uint32_t RUN[MAX_CLASSES], RUN_END[MAX_CLASSES];  // unused tail of the class's current chunk
static uint32_t top = HEAP_LO;
typedef struct { uint32_t lo, len; } Span;
static Span SPANS[MAX_SPANS];                         // free large spans, sorted by address, never adjacent
static int NSPANS;

static void heap_fatal(const char *what, uint32_t p) {
    fprintf(stderr, "heap: %s %#x\n", what, p);
    exit(6);
}

// 32..128 in steps of 16, then four classes per doubling up to SMALL_MAX.
__attribute__((constructor)) static void init_classes(void) {
    for (uint32_t s = 32; s <= 128; s += 16) CLASS_SIZE[NCLASSES++] = s;
    for (uint32_t base = 128; base < SMALL_MAX; base *= 2)
        for (uint32_t k = 1; k <= 4; k++) CLASS_SIZE[NCLASSES++] = base + base / 4 * k;
}

static uint32_t class_of(uint32_t bytes) {  // smallest class holding `bytes` (<= SMALL_MAX)
    uint32_t lo = 0, hi = NCLASSES - 1;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (CLASS_SIZE[mid] < bytes) lo = mid + 1; else hi = mid;
    }
    return lo;
}

// Let the host reclaim the whole host pages inside a free guest range. Their contents become undefined.
static void release(uint32_t lo, uint32_t len) {
    uint32_t hp = (uint32_t)getpagesize();
    uint32_t a = (lo + hp - 1) & ~(hp - 1), b = (lo + len) & ~(hp - 1);
    if (b > a && b - a >= 4 * hp) madvise(P(a), b - a, MADV_FREE);
}

static uint32_t span_alloc(uint32_t len) {  // len: multiple of PAGE; lock held
    for (int i = 0; i < NSPANS; i++) {
        if (SPANS[i].len < len) continue;
        uint32_t a = SPANS[i].lo;
        SPANS[i].lo += len;
        SPANS[i].len -= len;
        if (!SPANS[i].len) memmove(&SPANS[i], &SPANS[i + 1], (NSPANS-- - i - 1) * sizeof(Span));
        return a;
    }
    if (HEAP_HI - top < len) return 0;
    uint32_t a = top;
    top += len;
    return a;
}

static void span_free(uint32_t lo, uint32_t len) {  // lock held
    int i = 0, hi = NSPANS;  // i = first span above lo
    while (i < hi) {
        int mid = (i + hi) / 2;
        if (SPANS[mid].lo < lo) i = mid + 1; else hi = mid;
    }
    int prev = i > 0 && SPANS[i - 1].lo + SPANS[i - 1].len == lo;
    int next = i < NSPANS && lo + len == SPANS[i].lo;
    if (prev) { i--; SPANS[i].len += len; }
    if (next) {
        if (prev) { SPANS[i].len += SPANS[i + 1].len; memmove(&SPANS[i + 1], &SPANS[i + 2], (NSPANS-- - i - 2) * sizeof(Span)); }
        else { SPANS[i].lo = lo; SPANS[i].len += len; }
    }
    if (!prev && !next) {
        if (NSPANS == MAX_SPANS) heap_fatal("too many free spans at", lo);
        memmove(&SPANS[i + 1], &SPANS[i], (NSPANS++ - i) * sizeof(Span));
        SPANS[i] = (Span){lo, len};
    }
    release(lo, len);
    if (SPANS[i].lo + SPANS[i].len == top) {  // give the top span back to the bump pointer
        top = SPANS[i].lo;
        memmove(&SPANS[i], &SPANS[i + 1], (NSPANS-- - i - 1) * sizeof(Span));
    }
}

static uint32_t alloc_locked(uint32_t size) {
    if (size > HEAP_HI - HEAP_LO) return 0;
    uint32_t need = size + HDR, b, cls, blk;
    if (need <= SMALL_MAX) {
        cls = class_of(need);
        b = CLASS_SIZE[cls];
        if ((blk = FREE_LIST[cls])) {
            FREE_LIST[cls] = rd32(blk + HDR);
        } else {
            if (RUN_END[cls] - RUN[cls] < b) {
                uint32_t chunk = b <= 4096 ? 65536 : 8 * ((b + PAGE - 1) & ~(PAGE - 1));
                if (!(RUN[cls] = span_alloc(chunk))) return RUN_END[cls] = 0;
                RUN_END[cls] = RUN[cls] + chunk;
            }
            blk = RUN[cls];
            RUN[cls] += b;
        }
    } else {
        cls = LARGE;
        b = (need + PAGE - 1) & ~(PAGE - 1);
        if (!(blk = span_alloc(b))) return 0;
    }
    wr32(blk, size);
    wr32(blk + 4, cls);
    wr32(blk + 8, b);
    wr32(blk + 12, LIVE);
    return blk + HDR;
}

static uint32_t header(uint32_t p) {  // header address of live block p, or fatal
    if (p < HEAP_LO + HDR || p >= HEAP_HI || (p & 15) || rd32(p - HDR + 12) != LIVE) heap_fatal("bad pointer", p);
    return p - HDR;
}

static void free_locked(uint32_t p) {
    uint32_t h = header(p), cls = rd32(h + 4);
    wr32(h + 12, DEAD);
    if (cls == LARGE) {
        span_free(h, rd32(h + 8));
    } else {
        wr32(p, FREE_LIST[cls]);
        FREE_LIST[cls] = h;
    }
}

uint32_t heap_alloc(uint32_t size) {
    os_unfair_lock_lock(&LOCK);
    uint32_t p = alloc_locked(size);
    os_unfair_lock_unlock(&LOCK);
    return p;
}

uint32_t heap_calloc(uint32_t n, uint32_t size) {
    uint32_t total;
    if (__builtin_mul_overflow(n, size, &total)) return 0;
    uint32_t p = heap_alloc(total);
    if (p) memset(P(p), 0, total);
    return p;
}

void heap_free(uint32_t p) {
    if (!p) return;
    os_unfair_lock_lock(&LOCK);
    free_locked(p);
    os_unfair_lock_unlock(&LOCK);
}

uint32_t heap_size(uint32_t p) {
    return rd32(header(p));
}

int heap_owns(uint32_t p) {
    return p >= HEAP_LO + HDR && p < HEAP_HI && !(p & 15) && rd32(p - HDR + 12) == LIVE;
}

uint32_t heap_resize(uint32_t p, uint32_t size) {
    uint32_t h = header(p);
    if (size > HEAP_HI - HEAP_LO || size + HDR > rd32(h + 8)) return 0;
    wr32(h, size);
    return p;
}

uint32_t heap_realloc(uint32_t p, uint32_t size) {
    if (!p) return heap_alloc(size);
    uint32_t h = header(p), b = rd32(h + 8), old = rd32(h);
    if (size <= HEAP_HI - HEAP_LO && size + HDR <= b && (size + HDR) * 2 > b) {  // fits, not wasteful
        wr32(h, size);
        return p;
    }
    uint32_t q = heap_alloc(size);
    if (!q) return 0;
    memcpy(P(q), P(p), old < size ? old : size);
    heap_free(p);
    return q;
}

uint32_t heap_aligned_alloc(uint32_t size, uint32_t align) {
    if (align & (align - 1)) return 0;
    if (align < 4) align = 4;
    uint32_t extra = align - 1 + 4, total;
    if (__builtin_add_overflow(size, extra, &total)) return 0;
    uint32_t p = heap_alloc(total);
    if (!p) return 0;
    uint32_t r = (p + extra) & ~(align - 1);
    wr32(r - 4, p);
    return r;
}

void heap_aligned_free(uint32_t p) {
    if (p) heap_free(rd32((p & ~3u) - 4));
}
