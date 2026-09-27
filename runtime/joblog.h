// Job system log (PLAN3 Phase 9), MINA4MAC_JOBLOG=<file>: what the game's worker threads do per frame and how
// long the main thread waits for them. Off (and free, beyond one flag test in a few HLE calls) without the env var.
// Analysed by tools/joblog.py; the line format is described in joblog.c.
#pragma once
#include <stdint.h>

#include "cpu.h"

extern int joblog_on;

uint64_t joblog_now(void);                       // ns, CLOCK_UPTIME_RAW
void joblog_init(uint32_t exe_base);             // after imports are bound: open the log, hook the job functions
void joblog_thread(int slot, uint32_t start);    // _beginthreadex created guest thread `slot`
void joblog_poll(CPU *c);                        // the guest polled ConcRT's scheduler id (the job wait loop)
void joblog_sleep(uint64_t t0, uint64_t t1);     // the calling thread slept (Sleep/__crtSleep) from t0 to t1
void joblog_cond(uint64_t t0, uint64_t t1);      // the calling thread waited on a condition variable
void joblog_notify(int all);                     // the calling thread signalled a condition variable
void joblog_frame(uint64_t t0, uint64_t t1);     // SDL_GL_SwapWindow ran from t0 to t1
