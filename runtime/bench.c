// Microbenchmarks: recompiled guest functions vs hand-written native equivalents.
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "rt.h"

// The pre-page-table lookup, kept as the dispatch benchmark's reference.
__attribute__((noinline)) static GuestFn bsearch_fn(uint32_t a) {
    for (int lo = 0, hi = FN_COUNT - 1; lo <= hi;) {
        int mid = (lo + hi) / 2;
        if (FN_TABLE[mid].addr == a) return FN_TABLE[mid].fn;
        if (FN_TABLE[mid].addr < a) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

// Hand-written equivalent of guest 0xdda6b0 (Park-Miller RNG step on a double seed).
__attribute__((noinline)) static double rng_native(double *seed) {
    int32_t s = (int32_t)*seed;
    int32_t hi = s / 127773, lo = s % 127773;   // what the compiler's magic-number division computes
    int32_t r = 16807 * lo - 2836 * hi;
    (void)hi; (void)lo;
    r = (int32_t)(((int64_t)s * 16807) % 2147483647);
    if (r <= 0) r += 2147483647;
    *seed = r;
    return r * (1.0 / 2147483647.0);
}

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: bench <image.bin>\n"); return 2; }
    rt_init();
    rt_load_image(argv[1], 0x400000);
    const uint32_t SEED = 0x10000000, STACK = 0x08010000;
    CPU c = {0};
    GuestFn rng = rt_lookup(0xdda6b0);
    const int N = 50000000;

    // Recompiled: call it the way a guest caller would (push return address, ecx = &seed).
    wrf64(SEED, 12345.0);
    double t0 = now();
    double acc = 0;
    for (int i = 0; i < N; i++) {
        c.ecx = SEED; c.esp = STACK - 4; wr32(c.esp, 0);
        rng(&c);
        acc += c.xmm[0].f64[0];
    }
    double t1 = now();
    double s = 12345.0, acc2 = 0;
    for (int i = 0; i < N; i++) acc2 += rng_native(&s);
    double t2 = now();
    printf("rng recompiled %.2f ns/call, native %.2f ns/call (sum %.6f vs %.6f, seeds %.0f vs %.0f)\n",
           (t1 - t0) / N * 1e9, (t2 - t1) / N * 1e9, acc, acc2, rdf64(SEED), s);

    // Noise function 0xc3ef20 (float-heavy, calls helpers): recompiled cost per call.
    GuestFn noise = rt_lookup(0xc3ef20);
    const uint32_t OBJ = 0x10001000;
    for (int i = 0; i < 64; i++) wrf32(OBJ + 4 * i, 1.5f + i);
    const int M = 5000000;
    t0 = now();
    for (int i = 0; i < M; i++) {
        c.ecx = OBJ; c.esp = STACK - 4; wr32(c.esp, 0);
        wrf32(OBJ + 0x50, (float)i * 0.01f);
        noise(&c);
    }
    t1 = now();
    printf("noise recompiled %.2f ns/call (out %f)\n", (t1 - t0) / M * 1e9, rdf32(OBJ + 0x80));

    // guest_call dispatch: rt_lookup (page table) vs the old binary search over FN_TABLE, on random
    // function addresses (after one warm-up pass so every page is built).
    enum { K = 1 << 16 };
    static uint32_t targets[K];
    srand(1);
    for (int i = 0; i < K; i++) targets[i] = FN_TABLE[rand() % FN_COUNT].addr;
    const int R = 200;
    uintptr_t chk = 0, chk2 = 0;
    for (int i = 0; i < K; i++) chk ^= (uintptr_t)rt_lookup(targets[i]);
    t0 = now();
    for (int r = 0; r < R; r++)
        for (int i = 0; i < K; i++) chk ^= (uintptr_t)rt_lookup(targets[i]);
    t1 = now();
    for (int r = 0; r < R + 1; r++)
        for (int i = 0; i < K; i++) chk2 ^= (uintptr_t)bsearch_fn(targets[i]);
    t2 = now();
    printf("lookup page table %.2f ns, binary search %.2f ns (%d fns, %s)\n", (t1 - t0) / (R * K) * 1e9,
           (t2 - t1) / ((R + 1) * K) * 1e9, FN_COUNT, chk == chk2 ? "same results" : "MISMATCH");
    return 0;
}
