// Scheduling knobs (sched.h). The M1 Max has 8 performance and 2 efficiency cores; jobs that land on an E-core
// run several times slower and become the tail of the game's chunk-update passes (docs/PROFILE.md "Job system").
#include "sched.h"

#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/qos.h>
#include <sys/sysctl.h>
#include <unistd.h>

static uint32_t NCPU;  // 0 until sched_init (the tests don't call it): all CPUs
// User-interactive QoS keeps main (and the Box2D step it waits for) on the P-cores: jungle work_ms -8.2%, heavy
// and flood neutral (PLAN4 Phase 16). It only takes effect after sched_init, so the tests stay unspecified.
static qos_class_t QOS = QOS_CLASS_UNSPECIFIED;
// The game's job wait loop (while (pending) _Thrd_yield()) calls Sleep(0) 40-70k times a frame. usleep(0)
// returns at once, so main spins on a core the 9 workers need, pushing them onto the E-cores; a 20 µs nap
// frees it and notices the end of a 2-3 ms pass late by at most ~20 µs.
static enum { Y_USLEEP, Y_SCHED, Y_SPIN, Y_NAP } YIELD = Y_NAP;
static useconds_t NAP_US = 20;

static long sysctl_int(const char *name) {
    int v = 0;
    size_t n = sizeof v;
    return sysctlbyname(name, &v, &n, NULL, 0) ? -1 : v;
}

static const char *env(const char *name, const char *dflt) {
    const char *v = getenv(name);
    return v && *v ? v : dflt;
}

void sched_init(void) {
    const char *cpus = env("MINA4MAC_CPUS", "all");
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (!strcmp(cpus, "pcores")) {
        long p = sysctl_int("hw.perflevel0.logicalcpu");  // absent on Intel Macs: keep them all
        if (p > 0) n = p;
    } else if (strcmp(cpus, "all")) {
        n = atol(cpus);
    }
    NCPU = n < 1 ? 1 : n > 32 ? 32 : (uint32_t)n;

    const char *qos = env("MINA4MAC_QOS", "interactive");
    QOS = !strcmp(qos, "none")      ? QOS_CLASS_UNSPECIFIED
        : !strcmp(qos, "initiated") ? QOS_CLASS_USER_INITIATED
                                    : QOS_CLASS_USER_INTERACTIVE;

    const char *y = env("MINA4MAC_YIELD", "nap20");
    if (!strcmp(y, "sched")) YIELD = Y_SCHED;
    else if (!strcmp(y, "spin")) YIELD = Y_SPIN;
    else if (!strcmp(y, "usleep")) YIELD = Y_USLEEP;
    else if (!strncmp(y, "nap", 3)) YIELD = Y_NAP, NAP_US = (useconds_t)atol(y + 3);
    if (getenv("MINA4MAC_CPUS") || getenv("MINA4MAC_QOS") || getenv("MINA4MAC_YIELD"))
        fprintf(stderr, "[mina4mac] sched: %u cpus, qos %s, yield %s\n", NCPU, qos, y);
    sched_thread_start();
}

uint32_t sched_ncpu(void) {
    if (NCPU) return NCPU;
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n < 1 ? 1 : n > 32 ? 32 : (uint32_t)n;
}

void sched_thread_start(void) {
    if (QOS != QOS_CLASS_UNSPECIFIED) pthread_set_qos_class_self_np(QOS, 0);
}

void sched_sleep0(void) {
    switch (YIELD) {
    case Y_USLEEP: usleep(0); break;
    case Y_SCHED: sched_yield(); break;
    case Y_SPIN:  // about 1-2 µs of yield hints before giving up the core
        for (int i = 0; i < 64; i++) __builtin_arm_yield();
        sched_yield();
        break;
    case Y_NAP: usleep(NAP_US); break;
    }
}
