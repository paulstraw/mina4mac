// Scheduling knobs (PLAN3 Phase 9): the CPU count the guest sees, the QoS class of guest threads and how a
// zero-length sleep or yield waits. Each is set by an env var, read once at startup (sched_init):
//   MINA4MAC_CPUS=all|pcores|<n>                  CPU count for GetSystemInfo and ConcRT (sizes the job pool)
//   MINA4MAC_QOS=interactive|initiated|none        QoS class of the main thread and every guest thread
//                                                   (default interactive; none leaves it unspecified)
//   MINA4MAC_YIELD=nap<us>|usleep|sched|spin      Sleep(0)/_Thrd_yield: usleep(<us>) (default nap20), usleep(0),
//                                                   sched_yield, or a short spin of yield instructions then
//                                                   sched_yield
// The defaults are what won the PLAN3 Phase 9 A/Bs (PROFILE.md "Scheduling"): all CPUs and nap20; user-interactive
// QoS won PLAN4 Phase 16's (jungle work_ms -8.2%).
#pragma once
#include <stdint.h>

void sched_init(void);          // read the env vars; sets the calling (main) thread's QoS
uint32_t sched_ncpu(void);      // the CPU count to report, 1..32
void sched_thread_start(void);  // at the start of a guest thread: set its QoS
void sched_sleep0(void);        // Sleep(0) / a yield inside a spin-wait loop
