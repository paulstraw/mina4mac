// Job system log, MINA4MAC_JOBLOG=<file> (joblog.h). One line per event, times in µs since the log was opened:
//   cpus <n>                                   header: the CPU count GetSystemInfo reports
//   t <slot> <start>                           guest thread <slot> created, entry point <start>
//   j <slot> <fn> <inner> <t0> <t1> <cpu> <core0> <core1>
//                                              a std::function call on a guest thread other than main (the job
//                                              system runs every job through one): its _Do_call <fn>, the first
//                                              std::function called inside it (<inner>, 0 if none; it tells job
//                                              types apart, as <fn> is the same packaged task for all), wall
//                                              interval, thread CPU µs, and the core it started and ended on
//                                              (pthread_cpu_number_np; on the M1 Max 0-1 are the E-cores).
//                                              Only the outermost call per thread is logged.
//   w <slot> <site> <t0> <t1> <polls> <sleep>  a wait loop polling ConcRT's scheduler id (return address <site>;
//                                              0x726a5e is the chunk-update barrier): first poll to the return
//                                              of its last yield, the number of polls and the µs spent asleep
//   c <slot> <t0> <t1>                         a condition-variable wait (workers idle in these)
//   n <slot> <t> <all>                         a condition-variable notify (one or all)
//   f <t0> <t1>                                SDL_GL_SwapWindow (the frame boundary)
// Each thread buffers its lines and appends them in blocks (O_APPEND), so the file is ordered per thread only.
#include "joblog.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "proc.h"
#include "rt.h"
#include "sched.h"

int joblog_on;
static int FD = -1;
static uint64_t T0;

uint64_t joblog_now(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
static double us(uint64_t t) { return (double)(int64_t)(t - T0) / 1e3; }

typedef struct {
    char buf[1 << 16];
    int n, slot, depth;
    uint32_t inner;  // the first std::function called inside the current job
    uint64_t flushed;
    int wait_open, wait_polls;
    uint32_t wait_site;
    uint64_t wait_t0, wait_last, wait_sleep;
} Local;
static __thread Local *L;

static Local *local(void) {
    if (!L) {
        L = calloc(1, sizeof *L);
        if (!L) { perror("joblog"); exit(2); }
        L->slot = rt_thread_cpu ? THREAD_SLOT(rd32(rt_thread_cpu->fs_base + TEB_TID)) : -1;
        L->flushed = joblog_now();
    }
    return L;
}

static void flush(Local *l, uint64_t now) {
    if (l->n && write(FD, l->buf, l->n) < 0) joblog_on = 0;
    l->n = 0, l->flushed = now;
}

static void __attribute__((format(printf, 2, 3))) emit(Local *l, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    l->n += vsnprintf(l->buf + l->n, sizeof l->buf - l->n, fmt, ap);
    va_end(ap);
    uint64_t now = joblog_now();
    if (l->n > (int)sizeof l->buf - 256 || now - l->flushed > 200000000) flush(l, now);
}

static void close_wait(Local *l) {
    if (!l->wait_open) return;
    l->wait_open = 0;
    emit(l, "w %d %#x %.1f %.1f %d %.1f\n", l->slot, l->wait_site, us(l->wait_t0), us(l->wait_last), l->wait_polls,
         l->wait_sleep / 1e3);
}

void joblog_poll(CPU *c) {
    Local *l = local();
    uint64_t now = joblog_now();
    uint32_t site = rd32(c->esp);
    // The loop is poll, yield, poll...: a poll long after the last yield returned starts a new wait
    if (l->wait_open && (site != l->wait_site || now - l->wait_last > 20000)) close_wait(l);
    if (!l->wait_open) l->wait_open = 1, l->wait_site = site, l->wait_t0 = now, l->wait_polls = 0, l->wait_sleep = 0;
    l->wait_polls++, l->wait_last = now;
}

void joblog_sleep(uint64_t t0, uint64_t t1) {
    Local *l = local();
    if (l->wait_open) l->wait_last = t1, l->wait_sleep += t1 - t0;
}

void joblog_cond(uint64_t t0, uint64_t t1) {
    Local *l = local();
    emit(l, "c %d %.1f %.1f\n", l->slot, us(t0), us(t1));
}

void joblog_notify(int all) {
    Local *l = local();
    emit(l, "n %d %.1f %d\n", l->slot, us(joblog_now()), all);
}

void joblog_frame(uint64_t t0, uint64_t t1) {
    Local *l = local();
    close_wait(l);
    emit(l, "f %.1f %.1f\n", us(t0), us(t1));
}

void joblog_thread(int slot, uint32_t start) { emit(local(), "t %d %#x\n", slot, start); }

// Jobs: every std::function _Do_call (slot 2 of a _Func_impl vtable, found through RTTI) is replaced in the
// indirect-call table by one of the WRAP functions, each of which knows the index of its original in HOOKS.
enum { NHOOK = 1024 };
static struct { uint32_t fn; GuestFn orig; } HOOKS[NHOOK];
static int NHOOKED;

static double thread_cpu_us(void) { return clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID) / 1e3; }

static void job_call(CPU *c, int k) {
    GuestFn orig = HOOKS[k].orig;
    Local *l = local();
    if (l->slot <= 0 || l->depth) {
        if (l->depth == 1 && !l->inner) l->inner = HOOKS[k].fn;
        l->depth++;
        orig(c);
        l->depth--;
        return;
    }
    size_t core0 = 0, core1 = 0;
    pthread_cpu_number_np(&core0);
    double cpu = thread_cpu_us();
    uint64_t t0 = joblog_now();
    l->depth++, l->inner = 0;
    orig(c);
    l->depth--;
    uint64_t t1 = joblog_now();
    cpu = thread_cpu_us() - cpu;
    pthread_cpu_number_np(&core1);
    emit(l, "j %d %#x %#x %.1f %.1f %.1f %zu %zu\n", l->slot, HOOKS[k].fn, l->inner, us(t0), us(t1), cpu, core0, core1);
}

#define W1(k) static void wrap_##k(CPU *c) { job_call(c, 0x##k); }  // index = the name, in hex
#define W16(k) W1(k##0) W1(k##1) W1(k##2) W1(k##3) W1(k##4) W1(k##5) W1(k##6) W1(k##7) \
    W1(k##8) W1(k##9) W1(k##a) W1(k##b) W1(k##c) W1(k##d) W1(k##e) W1(k##f)
#define W256(k) W16(k##0) W16(k##1) W16(k##2) W16(k##3) W16(k##4) W16(k##5) W16(k##6) W16(k##7) \
    W16(k##8) W16(k##9) W16(k##a) W16(k##b) W16(k##c) W16(k##d) W16(k##e) W16(k##f)
W256(0) W256(1) W256(2) W256(3)
#define R1(k) wrap_##k,
#define R16(k) R1(k##0) R1(k##1) R1(k##2) R1(k##3) R1(k##4) R1(k##5) R1(k##6) R1(k##7) \
    R1(k##8) R1(k##9) R1(k##a) R1(k##b) R1(k##c) R1(k##d) R1(k##e) R1(k##f)
#define R256(k) R16(k##0) R16(k##1) R16(k##2) R16(k##3) R16(k##4) R16(k##5) R16(k##6) R16(k##7) \
    R16(k##8) R16(k##9) R16(k##a) R16(k##b) R16(k##c) R16(k##d) R16(k##e) R16(k##f)
static const GuestFn WRAP[NHOOK] = {R256(0) R256(1) R256(2) R256(3)};

static int in(uint32_t a, uint32_t lo, uint32_t hi) { return a >= lo && a < hi; }

static int hook_jobs(uint32_t base) {
    uint32_t nt = base + rd32(base + 0x3c), sec = nt + 24 + rd16(nt + 20), rlo = 0, rhi = 0, dlo = 0, dhi = 0;
    for (int i = 0; i < rd16(nt + 6); i++, sec += 40) {
        uint32_t lo = base + rd32(sec + 12), hi = lo + rd32(sec + 8);
        if (!memcmp(MEM + sec, ".rdata", 7)) rlo = lo, rhi = hi;
        if (!memcmp(MEM + sec, ".data", 6)) dlo = lo, dhi = hi;
    }
    static const char NAME[] = ".?AV?$_Func_impl@";
    int n = 0;
    for (uint32_t va = rlo + 4; va + 12 <= rhi; va += 4) {
        uint32_t col = rd32(va - 4), td = col + 12 < rhi && in(col, rlo, rhi) && !rd32(col) ? rd32(col + 12) : 0;
        if (!in(td, rlo, rhi) && !in(td, dlo, dhi)) continue;
        if (memcmp(MEM + td + 8, NAME, sizeof NAME - 1)) continue;
        uint32_t fn = rd32(va + 8);
        int k = 0;
        while (k < NHOOKED && HOOKS[k].fn != fn) k++;
        if (k < NHOOKED) continue;
        if (k == NHOOK) { fprintf(stderr, "joblog: more than %d std::function targets\n", NHOOK); exit(2); }
        HOOKS[k].fn = fn;
        if (!(HOOKS[k].orig = rt_hook(fn, WRAP[k]))) { fprintf(stderr, "joblog: no function at %#x\n", fn); exit(2); }
        NHOOKED++, n++;
    }
    return n;
}

void joblog_init(uint32_t exe_base) {
    const char *path = getenv("MINA4MAC_JOBLOG");
    if (!path || !*path) return;
    FD = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
    if (FD < 0) { perror(path); exit(2); }
    T0 = joblog_now();
    joblog_on = 1;
    int n = hook_jobs(exe_base);
    emit(local(), "cpus %ld\n", (long)sched_ncpu());
    fprintf(stderr, "[mina4mac] joblog: %s, %d std::function targets hooked\n", path, n);
}
