// Host synchronisation object pool (sync.h).
#include "sync.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "joblog.h"

enum { MAX_SYNC = 1 << 16 };
static Sync *POOL[MAX_SYNC + 1];  // index 0 unused: id 0 means "not initialised"
static uint32_t FREE[MAX_SYNC], NFREE, NUSED;
static pthread_mutex_t LOCK = PTHREAD_MUTEX_INITIALIZER;

uint32_t sync_new(void) {
    pthread_mutex_lock(&LOCK);
    uint32_t id = NFREE ? FREE[--NFREE] : NUSED < MAX_SYNC ? ++NUSED : 0;
    pthread_mutex_unlock(&LOCK);
    if (!id) { fprintf(stderr, "out of host sync objects\n"); exit(6); }
    Sync *s = POOL[id];
    if (!s) {
        s = malloc(sizeof *s);
        pthread_mutexattr_t a;
        pthread_mutexattr_init(&a);
        pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(&s->m, &a);
        pthread_cond_init(&s->cv, NULL);
        __atomic_store_n(&POOL[id], s, __ATOMIC_RELEASE);
    }
    s->signaled = 0;
    return id;
}

Sync *sync_get(uint32_t id) {
    Sync *s = id && id <= MAX_SYNC ? __atomic_load_n(&POOL[id], __ATOMIC_ACQUIRE) : NULL;
    if (!s) { fprintf(stderr, "guest sync object used uninitialised (id %u)\n", id); exit(6); }
    return s;
}

void sync_free(uint32_t id) {
    if (!id || id > MAX_SYNC) return;
    pthread_mutex_lock(&LOCK);
    FREE[NFREE++] = id;
    pthread_mutex_unlock(&LOCK);
}

static int timed_wait(Sync *s, pthread_mutex_t *m, uint32_t ms);
int sync_wait(Sync *s, pthread_mutex_t *m, uint32_t ms) {
    if (!joblog_on) return timed_wait(s, m, ms);
    uint64_t t0 = joblog_now();
    int r = timed_wait(s, m, ms);
    joblog_cond(t0, joblog_now());
    return r;
}

static int timed_wait(Sync *s, pthread_mutex_t *m, uint32_t ms) {
    if (ms == 0xffffffffu) return !pthread_cond_wait(&s->cv, m);
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000;
    if (ts.tv_nsec >= 1000000000) ts.tv_sec++, ts.tv_nsec -= 1000000000;
    return pthread_cond_timedwait(&s->cv, m, &ts) != ETIMEDOUT;
}
