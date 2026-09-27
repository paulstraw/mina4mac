// Guest threads (HLE): msvcr120 _beginthreadex/_endthreadex and the KERNEL32 functions on thread handles.
// Each guest thread is a host pthread with its own guest stack and TEB (proc.h). A thread handle names
// the thread by id (kernel32.h HK_THREAD); thread slots are never reused, so an id stays valid. Threads
// the launcher set up itself (the main thread) never exit as far as the guest can tell.
#include <pthread.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>

#include "host.h"
#include "joblog.h"
#include "kernel32.h"
#include "proc.h"
#include "sched.h"

enum { CREATE_SUSPENDED = 0x4, STILL_ACTIVE = 259, WAIT_OBJECT_0 = 0, WAIT_TIMEOUT = 0x102, WAIT_FAILED = 0xffffffffu };
enum { HOST_STACK = 64 << 20 };  // recompiled code keeps its C frames on the host stack

typedef struct {
    pthread_mutex_t m;
    pthread_cond_t cv;
    int started, done;
    uint32_t suspend, exit_code, start, arg;
} Thread;
static Thread THREADS[MAX_THREADS];
static __thread jmp_buf *EXIT_JMP;  // _endthreadex unwinds to the thread's entry

__attribute__((constructor)) static void threads_init(void) {
    for (int n = 0; n < MAX_THREADS; n++) pthread_mutex_init(&THREADS[n].m, NULL), pthread_cond_init(&THREADS[n].cv, NULL);
}

// A thread created by _beginthreadex, or one whose TEB is set up (the main thread).
static Thread *thread_of(uint32_t tid) {
    int n = THREAD_SLOT(tid);
    if (n < 0 || n >= MAX_THREADS) return NULL;
    int ok = __atomic_load_n(&THREADS[n].started, __ATOMIC_ACQUIRE) || rd32(TEBS_LO + TEB_SLOT * n + TEB_TID) == tid;
    return ok ? &THREADS[n] : NULL;
}

// The Thread for handle h (or the pseudo-handle for the current thread); NULL if it isn't one.
static Thread *thread_handle(CPU *c, uint32_t h) {
    uint64_t tid;
    if (h == H_CURRENT_THREAD) return thread_of(rd32(c->fs_base + TEB_TID));
    return handle_get(h, HK_THREAD, &tid) ? thread_of((uint32_t)tid) : NULL;
}

static void *thread_main(void *p) {
    int n = (int)(intptr_t)p;
    Thread *t = &THREADS[n];
    CPU c = {.fpu_cw = 0x27f};
    rt_thread_setup(&c, n);
    sched_thread_start();
    char name[32];  // shows in sample/Instruments, so profiles can tell guest threads apart (tools/perfprof.sh)
    snprintf(name, sizeof name, "guest %d %08x", n, t->start);
    pthread_setname_np(name);
    pthread_mutex_lock(&t->m);
    while (t->suspend) pthread_cond_wait(&t->cv, &t->m);
    pthread_mutex_unlock(&t->m);
    jmp_buf jb;
    EXIT_JMP = &jb;
    uint32_t code;
    if (!setjmp(jb)) code = call_guest(&c, t->start, 1, &t->arg);  // unsigned __stdcall start(void *)
    else code = t->exit_code;
    pthread_mutex_lock(&t->m);
    t->exit_code = code;
    t->done = 1;
    pthread_cond_broadcast(&t->cv);
    pthread_mutex_unlock(&t->m);
    return NULL;
}

// _beginthreadex(security, stack size, start, arg, flags, &thread id): returns a thread handle. The guest
// stack is always STACK_SIZE (proc.h), whatever size is asked for.
HOST_CDECL(msvcr120, _beginthreadex) {
    int n = rt_thread_reserve();
    Thread *t = &THREADS[n];
    t->start = ARG(2), t->arg = ARG(3), t->suspend = ARG(4) & CREATE_SUSPENDED ? 1 : 0;
    __atomic_store_n(&t->started, 1, __ATOMIC_RELEASE);
    uint32_t h = handle_new(HK_THREAD, THREAD_TID(n));
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, HOST_STACK);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_t pt;
    if (!h || pthread_create(&pt, &a, thread_main, (void *)(intptr_t)n)) {
        fprintf(stderr, "_beginthreadex: can't create a host thread\n");
        exit(6);
    }
    pthread_attr_destroy(&a);
    if (joblog_on) joblog_thread(n, t->start);
    if (ARG(5)) wr32(ARG(5), THREAD_TID(n));
    ret_i32(c, h);
}

HOST_CDECL(msvcr120, _endthreadex) {
    Thread *t = thread_of(rd32(c->fs_base + TEB_TID));
    if (!t || !EXIT_JMP) { fprintf(stderr, "_endthreadex on the main thread\n"); exit(ARG(0)); }
    t->exit_code = ARG(0);
    longjmp(*EXIT_JMP, 1);
}

HOST_STDCALL(kernel32, ResumeThread, 4) {  // returns the previous suspend count
    Thread *t = thread_handle(c, ARG(0));
    if (!t) return ret_i32(c, 0xffffffff);
    pthread_mutex_lock(&t->m);
    uint32_t prev = t->suspend;
    if (prev && !--t->suspend) pthread_cond_broadcast(&t->cv);
    pthread_mutex_unlock(&t->m);
    ret_i32(c, prev);
}

HOST_STDCALL(kernel32, GetExitCodeThread, 8) {  // (handle, &code)
    Thread *t = thread_handle(c, ARG(0));
    if (!t) return ret_i32(c, 0);
    pthread_mutex_lock(&t->m);
    wr32(ARG(1), t->done ? t->exit_code : STILL_ACTIVE);
    pthread_mutex_unlock(&t->m);
    ret_i32(c, 1);
}

// WaitForSingleObject(handle, ms). Only thread handles so far (signaled once the thread has exited).
HOST_STDCALL(kernel32, WaitForSingleObject, 8) {
    Thread *t = thread_handle(c, ARG(0));
    if (!t) { fprintf(stderr, "WaitForSingleObject: unsupported handle %#x\n", ARG(0)); exit(4); }
    uint32_t ms = ARG(1);
    pthread_mutex_lock(&t->m);
    if (ms == 0xffffffffu)
        while (!t->done) pthread_cond_wait(&t->cv, &t->m);
    else if (!t->done && ms) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += ms / 1000, ts.tv_nsec += (long)(ms % 1000) * 1000000;
        if (ts.tv_nsec >= 1000000000) ts.tv_sec++, ts.tv_nsec -= 1000000000;
        while (!t->done && !pthread_cond_timedwait(&t->cv, &t->m, &ts)) {}
    }
    int done = t->done;
    pthread_mutex_unlock(&t->m);
    ret_i32(c, done ? WAIT_OBJECT_0 : WAIT_TIMEOUT);
}
