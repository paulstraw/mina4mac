// Multithreaded test of recompiled locked instructions and memory ordering (built and run by
// tools/atomictest.py). Several host threads, each with its own CPU and guest stack, hammer shared
// guest words through the lifted instructions; any lost update shows up as a wrong final count.
// The ordering tests run game instructions lifted with x86 ordering (T_*_ord, which must pass) and
// without (T_*_plain, informational: they show what ARM64 and clang do to plain accesses).
//   atomic_test <threads> <iters> <rounds>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "rt.h"

void T_xadd(CPU *c);       // lock xadd [ecx], eax
void T_xchg(CPU *c);       // xchg [eax], esi
void T_cmpxchg(CPU *c);    // lock cmpxchg [esi], ecx
void T_cmpxchg8b(CPU *c);  // lock cmpxchg8b [esi]
void T_poll_ord(CPU *c), T_poll_plain(CPU *c);      // mov eax, [edi+0x1c]
void T_unlock_ord(CPU *c), T_unlock_plain(CPU *c);  // mov dword ptr [esi+0x18], 0
void T_spin_ord(CPU *c), T_spin_plain(CPU *c);      // while ([0x12056d0] != 2) {}

// Shared guest words, each on its own cache line.
enum { XADD = 0x1000, LOCK = 0x1040, GUARDED = 0x1080, CAS = 0x10c0, CAS8 = 0x1100, PLOCK = 0x1140, PGUARDED = 0x1180,
       JOB = 0x2000, ROUND = 0x2040, DATA = 0x2080, NDATA = 4, SPIN = 0x12056d0, STACKS = 0x100000 };
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

        // Spinlocks: xchg to take, the game's plain `mov [esi+0x18], 0` to release.
        do { c.esp = sp; c.eax = LOCK; c.esi = 1; T_xchg(&c); } while (c.esi != 0);
        wr32(GUARDED, rd32(GUARDED) + 1);
        c.esp = sp; c.esi = LOCK - 0x18; T_unlock_ord(&c);
        do { c.esp = sp; c.eax = PLOCK; c.esi = 1; T_xchg(&c); } while (c.esi != 0);
        wr32(PGUARDED, rd32(PGUARDED) + 1);
        c.esp = sp; c.esi = PLOCK - 0x18; T_unlock_plain(&c);

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

// Message passing, as in the game's job system: a worker writes its results, then drops the job's
// pending count with lock xadd; the waiting thread polls the count (0x726a51) and then reads the
// results. Returns how many rounds read a stale result.
static int ROUNDS;
static void *mp_writer(void *p) {
    (void)p;
    CPU c = {.fpu_cw = 0x27f, .esp = STACKS - 16};
    for (uint32_t r = 1; r <= (uint32_t)ROUNDS; r++) {
        while (__atomic_load_n((uint32_t *)P(ROUND), __ATOMIC_ACQUIRE) != r) {}
        for (int k = 0; k < NDATA; k++) wr32(DATA + 64 * k, r);
        c.ecx = JOB + 0x1c; c.eax = (uint32_t)-1;
        T_xadd(&c);
    }
    return NULL;
}

static int message_passing(void (*poll)(CPU *)) {
    pthread_t th;
    __atomic_store_n((uint32_t *)P(ROUND), 0, __ATOMIC_SEQ_CST);
    pthread_create(&th, NULL, mp_writer, NULL);
    CPU c = {.fpu_cw = 0x27f, .esp = STACKS + 0x10000 * 70 - 16};
    int stale = 0;
    for (uint32_t r = 1; r <= (uint32_t)ROUNDS; r++) {
        __atomic_store_n((uint32_t *)P(JOB + 0x1c), 1, __ATOMIC_RELAXED);
        __atomic_store_n((uint32_t *)P(ROUND), r, __ATOMIC_RELEASE);
        do { c.edi = JOB; poll(&c); } while (c.eax != 0);
        for (int k = 0; k < NDATA; k++) stale += rd32(DATA + 64 * k) != r;
    }
    pthread_join(th, NULL);
    return stale;
}

// The spin-wait at 0x84aba0 on another thread; returns 1 if it ended within 2 s of the store of 2.
static volatile int spin_done;
static void *spinner(void *p) {
    CPU c = {.fpu_cw = 0x27f, .esp = STACKS + 0x10000 * 71 - 16};
    ((void (*)(CPU *))p)(&c);
    spin_done = 1;
    return NULL;
}

static int spin_ends(void (*spin)(CPU *)) {
    pthread_t th;
    spin_done = 0;
    __atomic_store_n((uint32_t *)P(SPIN), 0, __ATOMIC_SEQ_CST);
    pthread_create(&th, NULL, spinner, (void *)spin);
    usleep(20000);
    __atomic_store_n((uint32_t *)P(SPIN), 2, __ATOMIC_RELEASE);
    for (int i = 0; i < 200 && !spin_done; i++) usleep(10000);
    if (spin_done) pthread_join(th, NULL);
    else pthread_detach(th);  // spins until exit
    return spin_done;
}

int main(int argc, char **argv) {
    if (argc != 4) return 2;
    int nt = atoi(argv[1]);
    ITERS = atoi(argv[2]);
    ROUNDS = atoi(argv[3]);
    rt_init();
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
    printf("plain unlock: %llu lost updates (informational)\n", (unsigned long long)(total - rd32(PGUARDED)));
    CHECK("cmpxchg", rd32(CAS), total);
    CHECK("cmpxchg8b", rd64(CAS8) - CAS8_START, total);
    CHECK("mp stale", message_passing(T_poll_ord), 0);
    printf("plain poll: %d stale of %d rounds (informational)\n", message_passing(T_poll_plain), ROUNDS);
    CHECK("spin ends", spin_ends(T_spin_ord), 1);
    printf("plain spin: %s (informational)\n", spin_ends(T_spin_plain) ? "ended" : "hung, the load was hoisted");
    printf("%d threads x %d iters, %d rounds: %s\n", nt, ITERS, ROUNDS, fails ? "FAIL" : "ok");
    return fails != 0;
}
