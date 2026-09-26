// Shared runtime: guest memory setup, image loading, guest function lookup and the guest_* hooks
// called by recompiled code.
#include "rt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
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

// Import thunks. Thunks are created while binding (single-threaded startup); a host implementation is
// looked up in the registry on a thunk's first call and cached.
enum { MAX_THUNKS = 1 << 16, MAX_HOST_FNS = 1 << 12 };
typedef struct { const char *dll, *name; GuestFn fn; } HostFn;
static HostFn THUNKS[MAX_THUNKS];
static int NTHUNKS;
static HostFn HOST_FNS[MAX_HOST_FNS];
static int NHOST_FNS;

static GuestFn find_host_fn(const char *dll, const char *name) {
    for (int i = 0; i < NHOST_FNS; i++)
        if (!strcasecmp(HOST_FNS[i].dll, dll) && !strcmp(HOST_FNS[i].name, name)) return HOST_FNS[i].fn;
    return NULL;
}

void rt_register_import(const char *dll, const char *name, GuestFn fn) {
    if (NHOST_FNS == MAX_HOST_FNS) { fprintf(stderr, "too many host functions\n"); exit(2); }
    HOST_FNS[NHOST_FNS++] = (HostFn){dll, name, fn};
}

uint32_t rt_thunk(const char *dll, const char *name) {
    int i = 0;
    while (i < NTHUNKS && !(strcasecmp(THUNKS[i].dll, dll) == 0 && strcmp(THUNKS[i].name, name) == 0)) i++;
    if (i == NTHUNKS) {
        if (NTHUNKS == MAX_THUNKS) { fprintf(stderr, "too many import thunks\n"); exit(2); }
        THUNKS[NTHUNKS++] = (HostFn){strdup(dll), strdup(name), NULL};
    }
    return THUNK_BASE + THUNK_STRIDE * i;
}

int rt_bind_imports(uint32_t base) {
    uint32_t nt = base + rd32(base + 0x3c);
    if (rd32(nt) != 0x4550 || rd16(nt + 24) != 0x10b) { fprintf(stderr, "%#x: not a PE32 image\n", base); exit(2); }
    uint32_t dir = rd32(nt + 24 + 96 + 8);  // optional header data directory 1: imports
    int n = 0;
    for (uint32_t d = base + dir; dir && rd32(d + 12); d += 20) {  // IMAGE_IMPORT_DESCRIPTOR
        const char *dll = (const char *)P(base + rd32(d + 12));
        uint32_t names = rd32(d) ? rd32(d) : rd32(d + 16);  // OriginalFirstThunk, else FirstThunk
        uint32_t iat = base + rd32(d + 16);
        for (uint32_t k = 0, e; (e = rd32(base + names + 4 * k)); k++) {
            char ord[16];
            const char *name = ord;
            if (e & 0x80000000u) snprintf(ord, sizeof ord, "#%u", e & 0xffff);
            else name = (const char *)P(base + e + 2);  // IMAGE_IMPORT_BY_NAME: hint, name
            wr32(iat + 4 * k, rt_thunk(dll, name));
            n++;
        }
    }
    return n;
}

static __attribute__((noinline)) void call_thunk(CPU *c, uint32_t target) {
    uint32_t i = (target - THUNK_BASE) / THUNK_STRIDE;
    if (target < THUNK_BASE || (target - THUNK_BASE) % THUNK_STRIDE || i >= (uint32_t)NTHUNKS) {
        fprintf(stderr, "no function at %#x\n", target);
        exit(5);
    }
    HostFn *t = &THUNKS[i];
    GuestFn f = __atomic_load_n(&t->fn, __ATOMIC_ACQUIRE);
    if (!f) {
        f = find_host_fn(t->dll, t->name);
        if (!f) {
            fprintf(stderr, "unimplemented import %s!%s (called from %#x)\n", t->dll, t->name, rd32(c->esp));
            exit(4);
        }
        __atomic_store_n(&t->fn, f, __ATOMIC_RELEASE);
    }
    f(c);
}

void guest_call(CPU *c, uint32_t target) {
    GuestFn f = rt_lookup(target);
    if (__builtin_expect(!f, 0)) call_thunk(c, target);
    else f(c);
}
void guest_unimpl(CPU *c, uint32_t addr, const char *what) {
    (void)c; fprintf(stderr, "unimpl %#x %s\n", addr, what); exit(3);
}
