// Guest process environment test (built and run by tools/check.sh): the guest heap directly, from many
// host threads, and through the MSVCR120/KERNEL32 imports; then the PEB, main-thread TEB, stack and
// static TLS set up for noita.exe; then the CRT startup imports (command line, initializer tables,
// exit handlers, _controlfp_s) and the KERNEL32 identity/time imports it calls.
//   env_test <noita image.bin>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "heap.h"
#include "host.h"
#include "msvcr120.h"
#include "proc.h"

// "Recompiled" guest functions for the initializer tables and exit handlers: each appends its tag to
// CALLS and returns (cdecl, no args) 0, except G_SEVEN which returns 7.
enum { G_A = 0x300000, G_B = 0x300010, G_SEVEN = 0x300020 };
static char CALLS[16];
static void guest_log(CPU *c, char tag, uint32_t r) {
    CALLS[strlen(CALLS)] = tag;
    c->eax = r;
    c->esp += 4;
}
static void F_a(CPU *c) { guest_log(c, 'a', 0); }
static void F_b(CPU *c) { guest_log(c, 'b', 0); }
static void F_seven(CPU *c) { guest_log(c, '7', 7); }
const FnEntry FN_TABLE[] = {{G_A, F_a}, {G_B, F_b}, {G_SEVEN, F_seven}};
const int FN_COUNT = 3;

enum { EXE_BASE = 0x400000, RET_ADDR = 0x0badf000, THREADS = 8, ITERS = 200000, LIVE_MAX = 256 };

static int fails;
#define CHECK(name, got, want)                                                                        \
    do {                                                                                              \
        uint64_t g_ = (got), w_ = (want);                                                             \
        printf("%-32s %s got %#llx want %#llx\n", name, g_ == w_ ? "ok  " : "FAIL", g_, w_);         \
        fails += g_ != w_;                                                                            \
    } while (0)

static int in_heap(uint32_t p, uint32_t n) { return p >= HEAP_LO && p + n <= HEAP_HI && p + n >= p; }

// Each thread keeps up to LIVE_MAX blocks filled with a per-block byte and checks them on free, so any
// overlap between blocks (from a race or an allocator bug) shows up as corruption.
static void *hammer(void *arg) {
    uint32_t seed = (uint32_t)(uintptr_t)arg * 2654435761u + 1, live[LIVE_MAX] = {0}, len[LIVE_MAX];
    long bad = 0;
    for (int i = 0; i < ITERS; i++) {
        seed = seed * 1103515245 + 12345;
        int k = (seed >> 8) % LIVE_MAX;
        uint8_t tag = (uint8_t)((uintptr_t)arg * 31 + k);
        if (live[k]) {
            for (uint32_t j = 0; j < len[k]; j++) bad += rd8(live[k] + j) != tag;
            if ((seed >> 20) & 1) { heap_free(live[k]); live[k] = 0; continue; }
            uint32_t n = (seed >> 4) % 3000;
            live[k] = heap_realloc(live[k], n);
            if (n > len[k]) memset(P(live[k] + len[k]), tag, n - len[k]);
            len[k] = n;
        } else {
            uint32_t n = (seed >> 12) & 1 ? (seed >> 4) % 200 : (seed >> 4) % 70000;
            live[k] = heap_alloc(n);
            bad += (live[k] & 15) || !in_heap(live[k], n);
            memset(P(live[k]), tag, n);
            len[k] = n;
        }
    }
    for (int k = 0; k < LIVE_MAX; k++) heap_free(live[k]);
    return (void *)bad;
}

// Guest-side call of dll!name with 4-byte args; returns esp after the call (before any caller cleanup).
static uint32_t call(CPU *c, const char *dll, const char *name, int n, const uint32_t *args) {
    uint32_t top = c->esp;
    for (int i = n - 1; i >= 0; i--) { c->esp -= 4; wr32(c->esp, args[i]); }
    c->esp -= 4;
    wr32(c->esp, RET_ADDR);
    guest_call(c, rt_thunk(dll, name));
    uint32_t sp = c->esp;
    c->esp = top;
    return sp;
}
#define CRT(name, ...) (call(&c, "MSVCR120.dll", name, sizeof((uint32_t[]){__VA_ARGS__}) / 4, (uint32_t[]){__VA_ARGS__}), c.eax)
#define K32(name, ...) (call(&c, "KERNEL32.dll", name, sizeof((uint32_t[]){__VA_ARGS__}) / 4, (uint32_t[]){__VA_ARGS__}), c.eax)

static int all(uint32_t p, uint8_t v, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) if (rd8(p + i) != v) return 0;
    return 1;
}

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: env_test <noita image.bin>\n"); return 2; }
    rt_init();

    // Heap, directly.
    uint32_t a = heap_alloc(0), b = heap_alloc(0);
    CHECK("malloc(0) unique non-null", a && b && a != b, 1);
    CHECK("small: aligned, in range", (a & 15) == 0 && in_heap(a, 16), 1);
    heap_free(a);
    CHECK("small: freed block reused", heap_alloc(0), a);
    uint32_t big = heap_alloc(1 << 20);
    CHECK("large: page + header aligned", big & 0xfff, 16);
    memset(P(big), 0xab, 1 << 20);
    uint32_t big2 = heap_realloc(big, 3 << 20);
    CHECK("realloc: contents kept", all(big2, 0xab, 1 << 20), 1);
    CHECK("realloc: size", heap_size(big2), 3 << 20);
    CHECK("realloc: shrink in place", heap_realloc(big2, 2 << 20), big2);
    uint32_t l1 = heap_alloc(100000), l2 = heap_alloc(100000), l3 = heap_alloc(100000);
    heap_free(l2); heap_free(l1); heap_free(l3);
    CHECK("large: spans coalesce", heap_alloc(300000), l1);
    uint32_t z = heap_calloc(1000, 7);
    CHECK("calloc: zeroed", all(z, 0, 7000), 1);
    CHECK("calloc: overflow fails", heap_calloc(0x10000, 0x10001), 0);
    CHECK("alloc: too big fails", heap_alloc(0xf0000000u), 0);
    CHECK("resize: in place fits", heap_resize(z, 7008), z);
    CHECK("resize: too big fails", heap_resize(z, 70000), 0);
    int aligned_ok = 1;
    for (uint32_t al = 1; al <= 65536; al *= 2) {
        uint32_t p = heap_aligned_alloc(1000, al);
        aligned_ok &= p && p % al == 0 && in_heap(p, 1000);
        memset(P(p), 1, 1000);
        heap_aligned_free(p);
    }
    CHECK("aligned alloc 1..64K", aligned_ok, 1);
    CHECK("aligned alloc: bad align", heap_aligned_alloc(10, 3), 0);
    CHECK("owns: live / freed", heap_owns(z) * 2 + heap_owns(l2), 2);

    // Heap, from many threads at once.
    pthread_t th[THREADS];
    for (long i = 0; i < THREADS; i++) pthread_create(&th[i], NULL, hammer, (void *)i);
    long bad = 0;
    for (int i = 0; i < THREADS; i++) { void *r; pthread_join(th[i], &r); bad += (long)r; }
    CHECK("threads: no corruption/overlap", bad, 0);

    // Process environment.
    rt_load_image(argv[1], EXE_BASE);
    rt_process_init(EXE_BASE);
    CPU c = {.fpu_cw = 0x27f};
    uint32_t teb = rt_thread_init(&c);
    CHECK("fs_base = TEB", c.fs_base, teb);
    CHECK("fs:[0] SEH chain end", rd32(teb + TEB_EXCEPTION_LIST), 0xffffffff);
    CHECK("fs:[4] stack base = esp", rd32(teb + TEB_STACK_BASE), c.esp);
    CHECK("stack size", rd32(teb + TEB_STACK_BASE) - rd32(teb + TEB_STACK_LIMIT), STACK_SIZE);
    CHECK("fs:[0x18] self", rd32(teb + TEB_SELF), teb);
    CHECK("fs:[0x24] thread id", rd32(teb + TEB_TID), GUEST_PID + 4);
    CHECK("fs:[0x30] PEB", rd32(teb + TEB_PEB), PEB_ADDR);
    CHECK("PEB image base", rd32(PEB_ADDR + PEB_IMAGE_BASE), EXE_BASE);
    CHECK("PEB process heap", rd32(PEB_ADDR + PEB_PROCESS_HEAP), PROCESS_HEAP);
    uint32_t nt = EXE_BASE + rd32(EXE_BASE + 0x3c), tlsdir = EXE_BASE + rd32(nt + 24 + 96 + 72);
    uint32_t tls_start = rd32(tlsdir), tls_len = rd32(tlsdir + 4) - tls_start;
    uint32_t block = rd32(rd32(teb + TEB_TLS_POINTER));
    CHECK("TLS index written", rd32(rd32(tlsdir + 8)), 0);
    CHECK("TLS block from heap", heap_owns(block), 1);
    CHECK("TLS block = template", memcmp(P(block), P(tls_start), tls_len), 0);
    CPU c2 = {0};
    uint32_t teb2 = rt_thread_init(&c2);
    CHECK("2nd thread: own TEB/stack/TLS", teb2 == teb + TEB_SLOT && c2.esp == c.esp + STACK_SLOT
          && rd32(rd32(teb2 + TEB_TLS_POINTER)) != block, 1);
    fflush(stdout);
    pid_t pid = fork();
    if (!pid) { wr32(rd32(teb + TEB_STACK_LIMIT) - 4, 1); _exit(0); }  // overflow into the guard
    int st;
    waitpid(pid, &st, 0);
    CHECK("stack guard faults", WIFSIGNALED(st) && (WTERMSIG(st) == SIGSEGV || WTERMSIG(st) == SIGBUS), 1);

    // The heap through the imports, called the way guest code calls them.
    uint32_t m = CRT("malloc", 40);
    CHECK("malloc", heap_owns(m) && heap_size(m) == 40, 1);
    memset(P(m), 7, 40);
    uint32_t m2 = CRT("realloc", m, 5000);
    CHECK("realloc keeps contents", all(m2, 7, 40), 1);
    CHECK("_msize", CRT("_msize", m2), 5000);
    CHECK("realloc(p, 0) frees", CRT("realloc", m2, 0) == 0 && !heap_owns(m2), 1);
    uint32_t cz = CRT("calloc", 10, 10);
    CHECK("calloc", all(cz, 0, 100), 1);
    CRT("free", cz);
    CHECK("free", heap_owns(cz), 0);
    uint32_t am = CRT("_aligned_malloc", 100, 256);
    CHECK("_aligned_malloc", am && am % 256 == 0, 1);
    CRT("_aligned_free", am);
    uint32_t n = CRT("??2@YAPAXI@Z", 24);
    CHECK("operator new", heap_owns(n), 1);
    CRT("??3@YAXPAX@Z", n);
    CHECK("operator delete", heap_owns(n), 0);
    CHECK("GetProcessHeap: stdcall pops 0", call(&c, "KERNEL32.dll", "GetProcessHeap", 0, NULL), c.esp);
    CHECK("GetProcessHeap", c.eax, PROCESS_HEAP);
    uint32_t h = K32("HeapAlloc", PROCESS_HEAP, 8, 64);
    CHECK("HeapAlloc ZERO_MEMORY", heap_owns(h) && all(h, 0, 64), 1);
    CHECK("HeapAlloc: stdcall pops 12", call(&c, "KERNEL32.dll", "HeapAlloc", 3, (uint32_t[]){PROCESS_HEAP, 0, 8}), c.esp);
    memset(P(h), 9, 64);
    uint32_t h2 = K32("HeapReAlloc", PROCESS_HEAP, 8, h, 128);
    CHECK("HeapReAlloc ZERO_MEMORY", all(h2, 9, 64) && all(h2 + 64, 0, 64), 1);
    CHECK("HeapReAlloc IN_PLACE fails", K32("HeapReAlloc", PROCESS_HEAP, 0x10, h2, 100000), 0);
    CHECK("HeapSize", K32("HeapSize", PROCESS_HEAP, 0, h2), 128);
    CHECK("HeapFree", K32("HeapFree", PROCESS_HEAP, 0, h2) == 1 && !heap_owns(h2), 1);

    // CRT startup.
    crt_init(3, (char *[]){"noitamac", "-x", "a b", NULL});
    uint32_t pargc = heap_alloc(12), pargv = pargc + 4, penv = pargc + 8;
    CHECK("__getmainargs", CRT("__getmainargs", pargc, pargv, penv, 0, 0), 0);
    uint32_t av = rd32(pargv);
    CHECK("argc/argv", rd32(pargc) == 3 && !strcmp((char *)P(rd32(av)), "noita.exe")
          && !strcmp((char *)P(rd32(av + 4)), "-x") && !strcmp((char *)P(rd32(av + 8)), "a b") && !rd32(av + 12), 1);
    CHECK("envp empty", rd32(rd32(penv)), 0);
    CHECK("_acmdln", strcmp((char *)P(rd32(rt_thunk("MSVCR120.dll", "_acmdln"))), "\"noita.exe\" -x \"a b\""), 0);
    uint32_t tab = heap_calloc(4, 4);
    wr32(tab, G_A); wr32(tab + 8, G_B);
    CRT("_initterm", tab, tab + 16);
    CHECK("_initterm: skips nulls", strcmp(CALLS, "ab"), 0);
    memset(CALLS, 0, sizeof CALLS);
    wr32(tab + 4, G_SEVEN);
    CHECK("_initterm_e: result", CRT("_initterm_e", tab, tab + 16), 7);
    CHECK("_initterm_e: stops at non-zero", strcmp(CALLS, "a7"), 0);
    memset(CALLS, 0, sizeof CALLS);
    CRT("_onexit", G_A); CRT("_onexit", G_B);
    CRT("_cexit");
    CHECK("_onexit/_cexit: reverse order", strcmp(CALLS, "ba"), 0);
    uint32_t cw = heap_alloc(4);
    CRT("_controlfp_s", cw, 0x300, 0x300);  // _RC_CHOP
    CHECK("_controlfp_s: chop", c.fpu_cw, 0xe7f);
    CHECK("_controlfp_s: current", rd32(cw), 0x9031f);  // all masked, _PC_53, _RC_CHOP
    CRT("_controlfp_s", 0, 0x20100, 0x30300);  // _PC_24, _RC_DOWN
    CHECK("_controlfp_s: pc24 down", c.fpu_cw, 0x47f);
    CRT("_controlfp_s", cw, 0, 0);
    CHECK("_controlfp_s: read only", rd32(cw) == 0xa011f && c.fpu_cw == 0x47f, 1);
    CRT("_controlfp_s", 0, 0x10000, 0x30300);
    CHECK("_controlfp_s: back to default", c.fpu_cw, 0x27f);

    // KERNEL32 identity and time.
    CHECK("GetCurrentThreadId", K32("GetCurrentThreadId"), GUEST_PID + 4);
    CHECK("GetCurrentProcessId", K32("GetCurrentProcessId"), GUEST_PID);
    K32("SetLastError", 1234);
    CHECK("Set/GetLastError", K32("GetLastError"), 1234);
    CHECK("SetLastError: stdcall pops 4", call(&c, "KERNEL32.dll", "SetLastError", 1, (uint32_t[]){0}), c.esp);
    CHECK("Encode/DecodePointer", K32("DecodePointer", K32("EncodePointer", 0x12345678)), 0x12345678);
    CHECK("IsProcessorFeaturePresent SSE2/3DNow", K32("IsProcessorFeaturePresent", 10) * 2
          + K32("IsProcessorFeaturePresent", 7), 2);
    uint32_t t = heap_alloc(16);
    K32("GetSystemTimeAsFileTime", t);
    CHECK("FILETIME after 2020", rd64(t) > 132223104000000000ull, 1);  // 2020-01-01
    K32("QueryPerformanceFrequency", t);
    uint64_t freq = rd64(t);
    K32("QueryPerformanceCounter", t); usleep(2000); K32("QueryPerformanceCounter", t + 8);
    uint64_t dt = (rd64(t + 8) - rd64(t)) * 1000 / freq;  // ms
    CHECK("QueryPerformanceCounter ~2 ms", dt >= 2 && dt < 100, 1);
    return fails != 0;
}
