// MSVCR120's Concurrency Runtime (ConcRT) synchronisation classes, implemented natively (HLE). Each
// object keeps a host sync object id (sync.h) in its first 4 bytes; the rest of its guest layout is
// unused. All methods are thiscall (`this` in ecx).
#include <sched.h>
#include <unistd.h>

#include "host.h"
#include "joblog.h"
#include "sched.h"
#include "sync.h"

static Sync *obj(uint32_t this) { return sync_get(rd32(this)); }
static void obj_init(CPU *c) { wr32(c->ecx, sync_new()); ret_i32(c, c->ecx); }
static void obj_free(CPU *c) { sync_free(rd32(c->ecx)); wr32(c->ecx, 0); }

// critical_section, _NonReentrantPPLLock and _ReentrantPPLLock are all a (recursive) host mutex.
#define LOCK_CLASS(id, cls)                                                                           \
    HOST(msvcr120, id##_ctor, "??0" cls "@@QAE@XZ", 0) { obj_init(c); }                               \
    HOST(msvcr120, id##_scoped_ctor, "??0_Scoped_lock@" cls "@@QAE@AAV123@@Z", 4) {                   \
        wr32(c->ecx, ARG(0));                                                                         \
        pthread_mutex_lock(&obj(ARG(0))->m);                                                          \
        ret_i32(c, c->ecx);                                                                           \
    }                                                                                                 \
    HOST(msvcr120, id##_scoped_dtor, "??1_Scoped_lock@" cls "@@QAE@XZ", 0) {                          \
        pthread_mutex_unlock(&obj(rd32(c->ecx))->m);                                                  \
    }
LOCK_CLASS(nonreentrant, "_NonReentrantPPLLock@details@Concurrency")
LOCK_CLASS(reentrant, "_ReentrantPPLLock@details@Concurrency")

HOST(msvcr120, cs_ctor, "??0critical_section@Concurrency@@QAE@XZ", 0) { obj_init(c); }
HOST(msvcr120, cs_dtor, "??1critical_section@Concurrency@@QAE@XZ", 0) { obj_free(c); }
HOST(msvcr120, cs_lock, "?lock@critical_section@Concurrency@@QAEXXZ", 0) { pthread_mutex_lock(&obj(c->ecx)->m); }
HOST(msvcr120, cs_unlock, "?unlock@critical_section@Concurrency@@QAEXXZ", 0) {
    pthread_mutex_unlock(&obj(c->ecx)->m);
}
HOST(msvcr120, cs_try_lock, "?try_lock@critical_section@Concurrency@@QAE_NXZ", 0) {
    ret_i32(c, !pthread_mutex_trylock(&obj(c->ecx)->m));
}
HOST(msvcr120, cs_try_lock_for, "?try_lock_for@critical_section@Concurrency@@QAE_NI@Z", 4) {  // (ms)
    pthread_mutex_t *m = &obj(c->ecx)->m;
    int ok = !pthread_mutex_trylock(m);
    for (uint32_t waited = 0; !ok && waited < ARG(0); waited++) {  // no timed lock on macOS: poll
        usleep(1000);
        ok = !pthread_mutex_trylock(m);
    }
    ret_i32(c, ok);
}

// _SpinLock(volatile long &flag): holds the flag at 1 for its lifetime.
HOST(msvcr120, spinlock_ctor, "??0_SpinLock@details@Concurrency@@QAE@ACJ@Z", 4) {
    wr32(c->ecx, ARG(0));
    for (uint32_t zero = 0; !__atomic_compare_exchange_n((uint32_t *)P(ARG(0)), &zero, 1, 0, __ATOMIC_ACQUIRE,
                                                        __ATOMIC_RELAXED); zero = 0)
        sched_yield();
    ret_i32(c, c->ecx);
}
HOST(msvcr120, spinlock_dtor, "??1_SpinLock@details@Concurrency@@QAE@XZ", 0) {
    __atomic_store_n((uint32_t *)P(rd32(c->ecx)), 0, __ATOMIC_RELEASE);
}

// event: manual reset, initially unset. wait returns 0, or COOPERATIVE_WAIT_TIMEOUT.
HOST(msvcr120, event_ctor, "??0event@Concurrency@@QAE@XZ", 0) { obj_init(c); }
HOST(msvcr120, event_dtor, "??1event@Concurrency@@QAE@XZ", 0) { obj_free(c); }
HOST(msvcr120, event_set, "?set@event@Concurrency@@QAEXXZ", 0) {
    Sync *s = obj(c->ecx);
    pthread_mutex_lock(&s->m);
    s->signaled = 1;
    pthread_cond_broadcast(&s->cv);
    pthread_mutex_unlock(&s->m);
}
HOST(msvcr120, event_reset, "?reset@event@Concurrency@@QAEXXZ", 0) {
    Sync *s = obj(c->ecx);
    pthread_mutex_lock(&s->m);
    s->signaled = 0;
    pthread_mutex_unlock(&s->m);
}
HOST(msvcr120, event_wait, "?wait@event@Concurrency@@QAEII@Z", 4) {  // (ms)
    Sync *s = obj(c->ecx);
    pthread_mutex_lock(&s->m);
    while (!s->signaled && sync_wait(s, &s->m, ARG(0))) {}
    int ok = s->signaled;
    pthread_mutex_unlock(&s->m);
    ret_i32(c, ok ? 0 : 0xffffffffu);
}

// _Condition_variable, used with a critical_section.
#define CV "_Condition_variable@details@Concurrency@@"
HOST(msvcr120, cv_ctor, "??0" CV "QAE@XZ", 0) { obj_init(c); }
HOST(msvcr120, cv_dtor, "??1" CV "QAE@XZ", 0) { obj_free(c); }
HOST(msvcr120, cv_wait, "?wait@" CV "QAEXAAVcritical_section@3@@Z", 4) {  // (critical_section &)
    sync_wait(obj(c->ecx), &obj(ARG(0))->m, 0xffffffffu);
}
HOST(msvcr120, cv_wait_for, "?wait_for@" CV "QAE_NAAVcritical_section@3@I@Z", 8) {  // (cs &, ms)
    ret_i32(c, sync_wait(obj(c->ecx), &obj(ARG(0))->m, ARG(1)));
}
HOST(msvcr120, cv_notify_one, "?notify_one@" CV "QAEXXZ", 0) {
    if (joblog_on) joblog_notify(0);
    pthread_cond_signal(&obj(c->ecx)->cv);
}
HOST(msvcr120, cv_notify_all, "?notify_all@" CV "QAEXXZ", 0) {
    if (joblog_on) joblog_notify(1);
    pthread_cond_broadcast(&obj(c->ecx)->cv);
}

// Scheduler queries and yields.
static uint32_t ncpu(void) { return sched_ncpu(); }
HOST(msvcr120, get_concurrency, "?_GetConcurrency@details@Concurrency@@YAIXZ", 0) { ret_i32(c, ncpu()); }
HOST(msvcr120, num_vprocs, "?GetNumberOfVirtualProcessors@CurrentScheduler@Concurrency@@SAIXZ", 0) {
    ret_i32(c, ncpu());
}
// No thread is attached to a ConcRT scheduler: the id is -1 (spin-waits then yield with msvcp _Thrd_yield).
HOST(msvcr120, current_scheduler_id, "?_Id@_CurrentScheduler@details@Concurrency@@SAIXZ", 0) {
    if (joblog_on) joblog_poll(c);
    ret_i32(c, 0xffffffff);
}
HOST(msvcr120, ctx_yield, "?_Yield@_Context@details@Concurrency@@SAXXZ", 0) { sched_yield(); }
HOST(msvcr120, underlying_yield, "?_UnderlyingYield@details@Concurrency@@YAXXZ", 0) { sched_yield(); }
HOST(msvcr120, concrt_wait, "?wait@Concurrency@@YAXI@Z", 0) { usleep(ARG(0) * 1000); }  // (ms)
