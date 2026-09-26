// Shared runtime: guest memory setup, image loading, guest function lookup and the guest_* hooks
// called by recompiled code.
#include "rt.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

uint8_t *MEM;

void rt_init(void) {
    MEM = mmap(NULL, 1ull << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (MEM == MAP_FAILED) { perror("mmap"); exit(2); }
}

void rt_load_image(const char *path, uint32_t base) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fread(MEM + base, 1, 64 << 20, f);
    fclose(f);
}

// Function lookup. Pages are built lazily (~2.5k of them cover noita.exe at 32 KB each, so building
// all up front would cost ~80 MB) and published with a CAS, so concurrent first lookups are safe.
enum { PAGE_BITS = 12, PAGE_FNS = 1 << PAGE_BITS, NPAGES = 1 << (32 - PAGE_BITS) };
static GuestFn *PAGES[NPAGES];
static GuestFn EMPTY_PAGE[PAGE_FNS];  // shared by every page without functions; never written

static int lower_bound(uint32_t a) {  // first FN_TABLE index with addr >= a
    int lo = 0, hi = FN_COUNT;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (FN_TABLE[mid].addr < a) lo = mid + 1; else hi = mid;
    }
    return lo;
}

static __attribute__((noinline)) GuestFn *build_page(uint32_t page) {
    uint32_t lo = page << PAGE_BITS;
    int i = lower_bound(lo);
    GuestFn *p = EMPTY_PAGE;
    if (i < FN_COUNT && FN_TABLE[i].addr >> PAGE_BITS == page) {
        p = calloc(PAGE_FNS, sizeof(GuestFn));
        if (!p) { perror("calloc"); exit(2); }
        for (; i < FN_COUNT && FN_TABLE[i].addr >> PAGE_BITS == page; i++) p[FN_TABLE[i].addr - lo] = FN_TABLE[i].fn;
    }
    GuestFn *expected = NULL;
    if (!__atomic_compare_exchange_n(&PAGES[page], &expected, p, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        if (p != EMPTY_PAGE) free(p);  // another thread won
        p = expected;
    }
    return p;
}

GuestFn rt_lookup(uint32_t a) {
    uint32_t page = a >> PAGE_BITS;
    GuestFn *p = __atomic_load_n(&PAGES[page], __ATOMIC_ACQUIRE);
    if (__builtin_expect(!p, 0)) p = build_page(page);
    return p[a & (PAGE_FNS - 1)];
}

void guest_call(CPU *c, uint32_t target) {
    GuestFn f = rt_lookup(target);
    if (!f) { fprintf(stderr, "no function at %#x\n", target); exit(5); }
    f(c);
}
void guest_import(CPU *c, uint32_t slot) { (void)c; fprintf(stderr, "import %#x\n", slot); exit(4); }
void guest_unimpl(CPU *c, uint32_t addr, const char *what) {
    (void)c; fprintf(stderr, "unimpl %#x %s\n", addr, what); exit(3);
}
