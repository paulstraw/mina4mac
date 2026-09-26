// Guest process environment: PEB, thread stacks, TEBs and static TLS.
#include "proc.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

#include "heap.h"

static uint32_t TLS_START, TLS_LEN, TLS_ZERO;  // static TLS template: raw data range and zero fill
static int NTHREADS;

void rt_process_init(uint32_t exe_base) {
    wr8(PEB_ADDR + PEB_BEING_DEBUGGED, 0);
    wr32(PEB_ADDR + PEB_IMAGE_BASE, exe_base);
    wr32(PEB_ADDR + PEB_PROCESS_HEAP, PROCESS_HEAP);
    wr32(PEB_ADDR + PEB_OS_MAJOR, 6);     // Windows 7 SP1
    wr32(PEB_ADDR + PEB_OS_MINOR, 1);
    wr16(PEB_ADDR + PEB_OS_BUILD, 7601);
    wr32(PEB_ADDR + PEB_OS_PLATFORM, 2);  // VER_PLATFORM_WIN32_NT

    uint32_t nt = exe_base + rd32(exe_base + 0x3c);
    uint32_t tls = rd32(nt + 24 + 96 + 8 * 9);  // optional header data directory 9: TLS
    if (tls) {
        tls += exe_base;
        TLS_START = rd32(tls);  // IMAGE_TLS_DIRECTORY32 holds VAs
        TLS_LEN = rd32(tls + 4) - TLS_START;
        TLS_ZERO = rd32(tls + 16);
        wr32(rd32(tls + 8), 0);  // *AddressOfIndex: the exe's TLS index is 0
    }
}

uint32_t rt_thread_init(CPU *c) {
    int n = __atomic_fetch_add(&NTHREADS, 1, __ATOMIC_RELAXED);
    if (n >= MAX_THREADS) { fprintf(stderr, "too many guest threads\n"); exit(2); }
    uint32_t guard = STACKS_LO + STACK_SLOT * n, lo = guard + STACK_GUARD, hi = lo + STACK_SIZE;
    if (mprotect(P(guard), STACK_GUARD, PROT_NONE)) { perror("mprotect"); exit(2); }

    uint32_t teb = TEBS_LO + TEB_SLOT * n, tls_array = teb + 0x1000;
    wr32(teb + TEB_EXCEPTION_LIST, 0xffffffff);
    wr32(teb + TEB_STACK_BASE, hi);
    wr32(teb + TEB_STACK_LIMIT, lo);
    wr32(teb + TEB_SELF, teb);
    wr32(teb + TEB_PID, GUEST_PID);
    wr32(teb + TEB_TID, GUEST_PID + 4 * (n + 1));
    wr32(teb + TEB_TLS_POINTER, tls_array);
    wr32(teb + TEB_PEB, PEB_ADDR);
    if (TLS_LEN + TLS_ZERO) {
        uint32_t block = heap_calloc(1, TLS_LEN + TLS_ZERO);
        if (!block) { fprintf(stderr, "out of guest heap for TLS\n"); exit(2); }
        memcpy(P(block), P(TLS_START), TLS_LEN);
        wr32(tls_array, block);
    }
    c->esp = hi;
    c->fs_base = teb;
    return teb;
}
