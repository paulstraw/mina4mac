// Shared runtime: guest memory setup, image loading, guest function lookup and the guest_* hooks
// called by recompiled code.
#include "rt.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>

void rt_init(void) {
    // A hint, not MAP_FIXED (which would silently replace whatever is there): check we got the address.
    void *m = mmap(MEM, 1ull << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (m == MAP_FAILED) { perror("mmap"); exit(2); }
    if (m != MEM) { fprintf(stderr, "guest memory mapped at %p, not %p\n", m, (void *)MEM); exit(2); }
}

void rt_load_image(const char *path, uint32_t base) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fread(MEM + base, 1, 64 << 20, f);
    fclose(f);
}

uint32_t rt_map_pe(const char *path, uint32_t base) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    uint8_t *file = malloc(size);
    rewind(f);
    if (!file || fread(file, 1, size, f) != (size_t)size) { perror(path); exit(2); }
    fclose(f);
#define F32(o) (*(u32u *)(file + (o)))
#define F16(o) (*(u16u *)(file + (o)))
    uint32_t nt = size > 0x40 ? F32(0x3c) : 0;
    if (nt + 0xf8 > (uint32_t)size || F32(nt) != 0x4550 || F16(nt + 24) != 0x10b) {
        fprintf(stderr, "%s: not a PE32 image\n", path);
        exit(2);
    }
    uint32_t opt = nt + 24, pref = F32(opt + 28), headers = F32(opt + 60), entry = F32(opt + 16);
    memcpy(MEM + base, file, headers);
    uint32_t sec = opt + F16(nt + 20);
    for (int i = 0; i < F16(nt + 6); i++, sec += 40) {  // IMAGE_SECTION_HEADER
        uint32_t va = F32(sec + 12), vsize = F32(sec + 8), raw = F32(sec + 16), off = F32(sec + 20);
        uint32_t n = raw < vsize || !vsize ? raw : vsize;
        if ((uint64_t)off + n > (uint64_t)size) { fprintf(stderr, "%s: truncated section\n", path); exit(2); }
        memcpy(MEM + base + va, file + off, n);
    }
#undef F32
#undef F16
    free(file);
    uint32_t delta = base - pref, rva = rd32(base + opt + 96 + 8 * 5), rsize = rd32(base + opt + 96 + 8 * 5 + 4);
    if (delta && !rva) { fprintf(stderr, "%s: no relocations, can't load at %#x\n", path, base); exit(2); }
    for (uint32_t b = base + rva, end = b + rsize; delta && b < end;) {  // IMAGE_BASE_RELOCATION blocks
        uint32_t page = base + rd32(b), bsize = rd32(b + 4);
        if (bsize < 8) break;
        for (uint32_t k = 8; k < bsize; k += 2) {
            uint16_t e = rd16(b + k);
            if (e >> 12 == 3) wr32(page + (e & 0xfff), rd32(page + (e & 0xfff)) + delta);  // HIGHLOW
            else if (e >> 12) { fprintf(stderr, "%s: relocation type %d\n", path, e >> 12); exit(2); }
        }
        b += bsize;
    }
    return base + entry;
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

// Targets that lifted call sites call directly when the target matches (build_all.py --ic), bypassing rt_lookup.
// build_all.py's table.c overrides these; test programs have none.
__attribute__((weak)) const uint32_t IC_TARGETS[1];
__attribute__((weak)) const int IC_TARGET_COUNT;

GuestFn rt_hook(uint32_t a, GuestFn fn) {
    for (int i = 0; i < IC_TARGET_COUNT; i++)
        if (IC_TARGETS[i] == a)
            fprintf(stderr, "rt_hook: %#x is an inline-cache target: calls from cached sites skip the hook "
                            "(build with --ic none)\n", a);
    uint32_t page = a >> PAGE_BITS;
    GuestFn *p = __atomic_load_n(&PAGES[page], __ATOMIC_ACQUIRE);
    if (!p) p = build_page(page);
    GuestFn old = p[a & (PAGE_FNS - 1)];
    if (old) p[a & (PAGE_FNS - 1)] = fn;
    return old;
}

// Import thunks. Thunks are created while binding (single-threaded startup); a host implementation is
// looked up in the registry on a thunk's first call and cached.
enum { MAX_THUNKS = 1 << 16, MAX_HOST_FNS = 1 << 12 };
typedef struct { const char *dll, *name; GuestFn fn; int traced; } HostFn;  // traced: thunks only
static HostFn THUNKS[MAX_THUNKS];
static int NTHUNKS;
static HostFn HOST_FNS[MAX_HOST_FNS];
static int NHOST_FNS;

static GuestFn find_host_fn(const char *dll, const char *name) {
    for (int i = 0; i < NHOST_FNS; i++)
        if (!strcasecmp(HOST_FNS[i].dll, dll) && !strcmp(HOST_FNS[i].name, name)) return HOST_FNS[i].fn;
    return NULL;
}

int rt_has_import(const char *dll, const char *name) { return find_host_fn(dll, name) != NULL; }

void rt_register_import(const char *dll, const char *name, GuestFn fn) {
    if (NHOST_FNS == MAX_HOST_FNS) { fprintf(stderr, "too many host functions\n"); exit(2); }
    HOST_FNS[NHOST_FNS++] = (HostFn){dll, name, fn, 0};
}

// Thunks may also be created later, by GetProcAddress on any thread: creation is serialised, and a new
// entry is filled in before NTHUNKS (read unlocked by call_thunk) covers it.
static pthread_mutex_t THUNKS_LOCK = PTHREAD_MUTEX_INITIALIZER;

uint32_t rt_thunk(const char *dll, const char *name) {
    pthread_mutex_lock(&THUNKS_LOCK);
    int i = 0;
    while (i < NTHUNKS && !(strcasecmp(THUNKS[i].dll, dll) == 0 && strcmp(THUNKS[i].name, name) == 0)) i++;
    if (i == NTHUNKS) {
        if (NTHUNKS == MAX_THUNKS) { fprintf(stderr, "too many import thunks\n"); exit(2); }
        THUNKS[i] = (HostFn){strdup(dll), strdup(name), NULL, rt_traced(dll, name)};
        __atomic_store_n(&NTHUNKS, i + 1, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&THUNKS_LOCK);
    return THUNK_BASE + THUNK_STRIDE * i;
}

enum { MAX_MODULES = 16 };
static struct { const char *dll; uint32_t base; } MODULES[MAX_MODULES];
static int NMODULES;

void rt_register_module(const char *dll, uint32_t base) {
    if (NMODULES == MAX_MODULES) { fprintf(stderr, "too many modules\n"); exit(2); }
    MODULES[NMODULES++] = (typeof(MODULES[0])){strdup(dll), base};
}

uint32_t rt_module_base(const char *dll) {
    for (int m = 0; m < NMODULES; m++)
        if (!strcasecmp(MODULES[m].dll, dll)) return MODULES[m].base;
    return 0;
}

uint32_t rt_export(uint32_t base, const char *name) {
    uint32_t nt = base + rd32(base + 0x3c), rva = rd32(nt + 24 + 96), size = rd32(nt + 24 + 96 + 4);
    if (!rva) return 0;
    uint32_t d = base + rva, n = rd32(d + 20), funcs = base + rd32(d + 28);  // IMAGE_EXPORT_DIRECTORY
    uint32_t i = 0xffffffffu;
    if (name[0] == '#') i = (uint32_t)atoi(name + 1) - rd32(d + 16);  // ordinal - Base
    else
        for (uint32_t k = 0, names = base + rd32(d + 32), ords = base + rd32(d + 36); k < rd32(d + 24); k++)
            if (!strcmp((const char *)P(base + rd32(names + 4 * k)), name)) { i = rd16(ords + 2 * k); break; }
    if (i >= n || !rd32(funcs + 4 * i)) return 0;
    uint32_t f = rd32(funcs + 4 * i);
    if (f >= rva && f < rva + size) { fprintf(stderr, "%s: forwarded exports not supported\n", name); exit(2); }
    return base + f;
}

int rt_bind_imports(uint32_t base) {
    uint32_t nt = base + rd32(base + 0x3c);
    if (rd32(nt) != 0x4550 || rd16(nt + 24) != 0x10b) { fprintf(stderr, "%#x: not a PE32 image\n", base); exit(2); }
    uint32_t dir = rd32(nt + 24 + 96 + 8);  // optional header data directory 1: imports
    int n = 0;
    for (uint32_t d = base + dir; dir && rd32(d + 12); d += 20) {  // IMAGE_IMPORT_DESCRIPTOR
        const char *dll = (const char *)P(base + rd32(d + 12));
        uint32_t names = rd32(d) ? rd32(d) : rd32(d + 16);  // OriginalFirstThunk, else FirstThunk
        uint32_t iat = base + rd32(d + 16), mod = rt_module_base(dll);
        for (uint32_t k = 0, e; (e = rd32(base + names + 4 * k)); k++) {
            char ord[16];
            const char *name = ord;
            if (e & 0x80000000u) snprintf(ord, sizeof ord, "#%u", e & 0xffff);
            else name = (const char *)P(base + e + 2);  // IMAGE_IMPORT_BY_NAME: hint, name
            uint32_t a = mod ? rt_export(mod, name) : rt_thunk(dll, name);
            if (!a) { fprintf(stderr, "%s has no export %s\n", dll, name); exit(2); }
            wr32(iat + 4 * k, a);
            n++;
        }
    }
    return n;
}

int rt_trace;
const char *rt_trace_filter;

int rt_traced(const char *dll, const char *name) {
    if (!rt_trace) return 0;
    if (!rt_trace_filter) return 1;
    char full[256];
    snprintf(full, sizeof full, "%s!%s", dll, name);
    for (const char *f = rt_trace_filter; *f;) {  // comma-separated substrings of dll!name
        size_t n = strcspn(f, ",");
        for (const char *p = full; *p; p++)
            if (n && !strncasecmp(p, f, n)) return 1;
        f += n + (f[n] == ',');
    }
    return 0;
}

// Per-thunk call counts, kept only after rt_count_imports; written as "calls<TAB>dll!name" lines at exit.
static uint64_t CALLS[MAX_THUNKS];
static const char *COUNT_PATH;

static void write_counts(void) {
    FILE *f = fopen(COUNT_PATH, "w");
    if (!f) { perror(COUNT_PATH); return; }
    for (int i = 0, n = __atomic_load_n(&NTHUNKS, __ATOMIC_ACQUIRE); i < n; i++)
        fprintf(f, "%llu\t%s!%s\n", (unsigned long long)__atomic_load_n(&CALLS[i], __ATOMIC_RELAXED), THUNKS[i].dll,
                THUNKS[i].name);
    fclose(f);
}

void rt_count_imports(const char *path) {
    COUNT_PATH = strdup(path);
    atexit(write_counts);
}

static void trace_call(CPU *c, HostFn *t, GuestFn f) {
    uint32_t sp = c->esp;
    fprintf(stderr, "[import] %s!%s(%#x, %#x, %#x, %#x) from %#x\n", t->dll, t->name, rd32(sp + 4), rd32(sp + 8),
            rd32(sp + 12), rd32(sp + 16), rd32(sp));
    if (!f) return;
    f(c);
    fprintf(stderr, "[import] %s!%s -> eax=%#x edx=%#x popped %u\n", t->dll, t->name, c->eax, c->edx, c->esp - sp);
}

static __attribute__((noinline)) void call_thunk(CPU *c, uint32_t target) {
    uint32_t i = (target - THUNK_BASE) / THUNK_STRIDE;
    if (target < THUNK_BASE || (target - THUNK_BASE) % THUNK_STRIDE || i >= (uint32_t)__atomic_load_n(&NTHUNKS, __ATOMIC_ACQUIRE)) {
        fprintf(stderr, "no function at %#x\n", target);
        exit(5);
    }
    HostFn *t = &THUNKS[i];
    if (COUNT_PATH) __atomic_fetch_add(&CALLS[i], 1, __ATOMIC_RELAXED);
    GuestFn f = __atomic_load_n(&t->fn, __ATOMIC_ACQUIRE);
    if (!f) {
        f = find_host_fn(t->dll, t->name);
        if (!f) {
            if (t->traced) trace_call(c, t, NULL);
            fprintf(stderr, "unimplemented import %s!%s (called from %#x)\n", t->dll, t->name, rd32(c->esp));
            exit(4);
        }
        __atomic_store_n(&t->fn, f, __ATOMIC_RELEASE);
    }
    if (t->traced) trace_call(c, t, f);
    else f(c);
}

void guest_call(CPU *c, uint32_t target) {
    GuestFn f = rt_lookup(target);
    if (__builtin_expect(!f, 0)) call_thunk(c, target);
    else f(c);
}

// Indirect-call site profile (build_all.py --icprof): the lifter calls guest_call_site with the call's address.
// Counts per (site, target) pair live in an open-addressing table; written as "site<TAB>target<TAB>calls" lines.
enum { ICPROF_SLOTS = 1 << 20 };
static uint64_t ICPROF_KEY[ICPROF_SLOTS], ICPROF_N[ICPROF_SLOTS];
static const char *ICPROF_PATH;

void guest_call_site(CPU *c, uint32_t target, uint32_t site) {
    if (ICPROF_PATH) {
        uint64_t key = (uint64_t)site << 32 | target;
        for (uint32_t h = (uint32_t)((key * 0x9E3779B97F4A7C15ull) >> 44);; h = (h + 1) & (ICPROF_SLOTS - 1)) {
            uint64_t k = __atomic_load_n(&ICPROF_KEY[h], __ATOMIC_ACQUIRE);
            if (!k && __atomic_compare_exchange_n(&ICPROF_KEY[h], &k, key, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) k = key;
            if (k == key) { __atomic_fetch_add(&ICPROF_N[h], 1, __ATOMIC_RELAXED); break; }
        }
    }
    guest_call(c, target);
}

// Register syncs around calls (--icprof builds): [stores, reloads] x [syncs, registers]. Each thread adds into its own
// counters and flushes them every 2^16 syncs, so a thread's last partial batch is lost (a rounding error).
static uint64_t SYNC_TOTAL[2][2];
static _Thread_local uint64_t SYNC_ACC[2][2];

void sync_count(uint32_t reload, uint32_t n) {
    SYNC_ACC[reload][0]++;
    SYNC_ACC[reload][1] += n;
    if (!(SYNC_ACC[reload][0] & 0xffff)) {
        __atomic_fetch_add(&SYNC_TOTAL[reload][0], SYNC_ACC[reload][0], __ATOMIC_RELAXED);
        __atomic_fetch_add(&SYNC_TOTAL[reload][1], SYNC_ACC[reload][1], __ATOMIC_RELAXED);
        SYNC_ACC[reload][0] = SYNC_ACC[reload][1] = 0;
    }
}

void sync_fail(uint32_t site, const char *reg, uint32_t actual, uint32_t expected) {
    fprintf(stderr, "sync check: after the call at %#x, %s is %#x but its callee's summary says %#x\n", site, reg, actual,
            expected);
    exit(12);
}

void slot_fail(uint32_t site, uint32_t addr, uint32_t actual, uint32_t expected) {
    fprintf(stderr, "slot check: at %#x, the stack slot at %#x holds %#x in memory but %#x in its local\n", site, addr,
            actual, expected);
    exit(13);
}

void slot_where(uint32_t site, uint32_t addr, uint32_t planned) {
    fprintf(stderr, "slot check: at %#x, the access is at %#x but the plan put its slot at %#x\n", site, addr, planned);
    exit(13);
}

void rt_icprof_write(void) {
    if (!ICPROF_PATH) return;
    char sp[1024];
    snprintf(sp, sizeof sp, "%s.sync", ICPROF_PATH);
    FILE *sf = fopen(sp, "w");
    if (sf) {
        for (int r = 0; r < 2; r++)
            fprintf(sf, "%s\t%llu syncs\t%llu registers\n", r ? "reloads" : "stores",
                    (unsigned long long)__atomic_load_n(&SYNC_TOTAL[r][0], __ATOMIC_RELAXED),
                    (unsigned long long)__atomic_load_n(&SYNC_TOTAL[r][1], __ATOMIC_RELAXED));
        fclose(sf);
    }
    FILE *f = fopen(ICPROF_PATH, "w");
    if (!f) { perror(ICPROF_PATH); return; }
    for (int i = 0; i < ICPROF_SLOTS; i++) {
        uint64_t k = __atomic_load_n(&ICPROF_KEY[i], __ATOMIC_ACQUIRE);
        if (k) fprintf(f, "%#x\t%#x\t%llu\n", (uint32_t)(k >> 32), (uint32_t)k,
                       (unsigned long long)__atomic_load_n(&ICPROF_N[i], __ATOMIC_RELAXED));
    }
    fclose(f);
}

void rt_icprof(const char *path) {
    ICPROF_PATH = strdup(path);
    atexit(rt_icprof_write);
}
void guest_unimpl(CPU *c, uint32_t addr, const char *what) {
    (void)c; fprintf(stderr, "unimpl %#x %s\n", addr, what); exit(3);
}
