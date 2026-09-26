// Host synchronisation objects backing guest locks, events and condition variables. A guest object keeps
// only a small id (>= 1) into this pool, so its guest layout doesn't matter.
#pragma once
#include <pthread.h>
#include <stdint.h>

typedef struct {
    pthread_mutex_t m;   // recursive
    pthread_cond_t cv;
    int signaled;        // for events
} Sync;

uint32_t sync_new(void);     // a fresh object (unlocked, not signaled); exits if the pool is full
Sync *sync_get(uint32_t id);  // exits on a bad id (an object used uninitialised)
void sync_free(uint32_t id);

// Wait on s->cv with s->m held, for `ms` milliseconds (0xFFFFFFFF: forever). Returns 0 on timeout.
int sync_wait(Sync *s, pthread_mutex_t *m, uint32_t ms);
