// Multithreaded test of recompiled locked instructions (built and run by tools/atomictest.py).
// Several host threads, each with its own CPU and guest stack, hammer shared guest words through
// the lifted instructions; any lost update shows up as a wrong final count.
//   atomic_test <threads> <iters>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

#include "cpu.h"

void T_xadd(CPU *c);       // lock xadd [ecx], eax
void T_xchg(CPU *c);       // xchg [eax], esi
void T_cmpxchg(CPU *c);    // lock cmpxchg [esi], ecx
void T_cmpxchg8b(CPU *c);  // lock cmpxchg8b [esi]

uint8_t *MEM;
void guest_call(CPU *c, uint32_t t) { (void)c; fprintf(stderr, "guest_call %#x\n", t); exit(5); }
void guest_import(CPU *c, uint32_t s) { (void)c; fprintf(stderr, "import %#x\n", s); exit(4); }
void guest_unimpl(CPU *c, uint32_t a, const char *w) { (void)c; fprintf(stderr, "unimpl %#x %s\n", a, w); exit(3); }

// Shared guest words, each on its own cache line.
enum { XADD = 0x1000, LOCK = 0x1040, GUARDED = 0x1080, CAS = 0x10c0, CAS8 = 0x1100, STACKS = 0x100000 };
#define CAS8_START 0xffffff00ull  // so the 64-bit counter carries into the high dword
static int ITERS;

typedef struct { int id; uint64_t xadd_sum; } Arg;

static void *worker(void *p) {
    Arg *arg = p;
    CPU c = {.fpu_cw = 0x27f, .esp = STACKS + 0x10000 * (arg->id + 1) - 16};
    uint32_t sp = c.esp;
    for (int i = 0; i < ITERS; i++) {
        c.esp = sp; c.ecx = XADD; c.eax = 1;
        T_xadd(&c);
        arg->xadd_sum += c.eax;  // old value; all old values together must be 0..total-1

        do { c.esp = sp; c.eax = LOCK; c.esi = 1; T_xchg(&c); } while (c.esi != 0);  // spinlock
        wr32(GUARDED, rd32(GUARDED) + 1);
        __atomic_store_n((uint32_t *)P(LOCK), 0, __ATOMIC_RELEASE);

        for (;;) {
            uint32_t old = __atomic_load_n((uint32_t *)P(CAS), __ATOMIC_RELAXED);
            c.esp = sp; c.esi = CAS; c.eax = old; c.ecx = old + 1;
            T_cmpxchg(&c);
            if (c.eax == old) break;  // on failure eax receives the (different) current value
        }

        for (;;) {
            uint64_t old = __atomic_load_n((uint64_t *)P(CAS8), __ATOMIC_RELAXED);
            c.esp = sp; c.esi = CAS8;
            c.eax = (uint32_t)old; c.edx = (uint32_t)(old >> 32);
            c.ebx = (uint32_t)(old + 1); c.ecx = (uint32_t)((old + 1) >> 32);
            T_cmpxchg8b(&c);
            if ((((uint64_t)c.edx << 32) | c.eax) == old) break;
        }
    }
    return NULL;
}

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    int nt = atoi(argv[1]);
    ITERS = atoi(argv[2]);
    MEM = mmap(NULL, 1ull << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (MEM == MAP_FAILED) { perror("mmap"); return 2; }
    wr64(CAS8, CAS8_START);
    pthread_t th[64];
    Arg args[64];
    for (int t = 0; t < nt; t++) {
        args[t] = (Arg){.id = t};
        pthread_create(&th[t], NULL, worker, &args[t]);
    }
    uint64_t sum = 0;
    for (int t = 0; t < nt; t++) { pthread_join(th[t], NULL); sum += args[t].xadd_sum; }
    uint64_t total = (uint64_t)nt * ITERS;
    int fails = 0;
#define CHECK(name, got, want)                                                                  \
    do {                                                                                        \
        uint64_t g_ = (got), w_ = (want);                                                       \
        printf("%-10s %s got %llu want %llu\n", name, g_ == w_ ? "ok  " : "FAIL",               \
               (unsigned long long)g_, (unsigned long long)w_);                                 \
        fails += g_ != w_;                                                                      \
    } while (0)
    CHECK("xadd", rd32(XADD), total);
    CHECK("xadd olds", sum, total * (total - 1) / 2);
    CHECK("xchg lock", rd32(GUARDED), total);
    CHECK("cmpxchg", rd32(CAS), total);
    CHECK("cmpxchg8b", rd64(CAS8) - CAS8_START, total);
    printf("%d threads x %d iters: %s\n", nt, ITERS, fails ? "FAIL" : "ok");
    return fails != 0;
}
