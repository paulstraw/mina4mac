// KERNEL32 implemented natively (HLE). Every heap handle maps to the one guest heap (heap.c).
#include "kernel32.h"

#include <pthread.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "heap.h"
#include "hle.h"
#include "host.h"
#include "proc.h"
#include "sync.h"

enum { HEAP_ZERO_MEMORY = 0x8, HEAP_REALLOC_IN_PLACE_ONLY = 0x10 };

HOST_STDCALL(kernel32, GetProcessHeap, 0) { ret_i32(c, PROCESS_HEAP); }

HOST_STDCALL(kernel32, HeapAlloc, 12) {  // (heap, flags, bytes)
    ret_i32(c, ARG(1) & HEAP_ZERO_MEMORY ? heap_calloc(1, ARG(2)) : heap_alloc(ARG(2)));
}

HOST_STDCALL(kernel32, HeapFree, 12) {  // (heap, flags, mem)
    heap_free(ARG(2));
    ret_i32(c, 1);
}

HOST_STDCALL(kernel32, HeapReAlloc, 16) {  // (heap, flags, mem, bytes)
    uint32_t flags = ARG(1), p = ARG(2), size = ARG(3);
    if (!p) return ret_i32(c, 0);
    uint32_t old = heap_size(p);
    uint32_t q = flags & HEAP_REALLOC_IN_PLACE_ONLY ? heap_resize(p, size) : heap_realloc(p, size);
    if (q && (flags & HEAP_ZERO_MEMORY) && size > old) memset(P(q + old), 0, size - old);
    ret_i32(c, q);
}

HOST_STDCALL(kernel32, HeapSize, 12) { ret_i32(c, heap_size(ARG(2))); }  // (heap, flags, mem)

// Process and thread identity. Ids match the TEB (proc.c); the pseudo-handles are Windows' own values.
HOST_STDCALL(kernel32, GetCurrentProcessId, 0) { ret_i32(c, GUEST_PID); }
HOST_STDCALL(kernel32, GetCurrentThreadId, 0) { ret_i32(c, rd32(c->fs_base + TEB_TID)); }
HOST_STDCALL(kernel32, IsDebuggerPresent, 0) { ret_i32(c, 0); }

HOST_STDCALL(kernel32, GetLastError, 0) { ret_i32(c, rd32(c->fs_base + TEB_LAST_ERROR)); }
HOST_STDCALL(kernel32, SetLastError, 4) { wr32(c->fs_base + TEB_LAST_ERROR, ARG(0)); }

// Windows XORs with a per-process secret; the identity is an equally valid encoding.
HOST_STDCALL(kernel32, EncodePointer, 4) { ret_i32(c, ARG(0)); }
HOST_STDCALL(kernel32, DecodePointer, 4) { ret_i32(c, ARG(0)); }

// PF_* features of the CPU cpuid_fixed describes: MMX, SSE, SSE2, cmpxchg8b, rdtsc.
HOST_STDCALL(kernel32, IsProcessorFeaturePresent, 4) {
    enum { PF_COMPARE_EXCHANGE_DOUBLE = 2, PF_MMX = 3, PF_XMMI = 6, PF_RDTSC = 8, PF_XMMI64 = 10 };
    uint32_t f = ARG(0);
    ret_i32(c, f == PF_COMPARE_EXCHANGE_DOUBLE || f == PF_MMX || f == PF_XMMI || f == PF_RDTSC || f == PF_XMMI64);
}

// Time. FILETIME is 100 ns units since 1601-01-01 UTC; the performance counter ticks in nanoseconds.
enum { FILETIME_UNIX_EPOCH_SECS = 11644473600u };
HOST_STDCALL(kernel32, GetSystemTimeAsFileTime, 4) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    wr64(ARG(0), ((uint64_t)ts.tv_sec + FILETIME_UNIX_EPOCH_SECS) * 10000000 + ts.tv_nsec / 100);
}
HOST_STDCALL(kernel32, QueryPerformanceCounter, 4) {
    wr64(ARG(0), clock_gettime_nsec_np(CLOCK_UPTIME_RAW));
    ret_i32(c, 1);
}
HOST_STDCALL(kernel32, QueryPerformanceFrequency, 4) {
    wr64(ARG(0), 1000000000);
    ret_i32(c, 1);
}

// Critical sections. A guest CRITICAL_SECTION (24 bytes) is backed by a host sync object (sync.h), whose
// id is kept in its LockSemaphore field. OwningThread and RecursionCount are kept up to date for guest
// code that inspects them; LockCount is not.
enum { CS_RECURSION = 8, CS_OWNER = 12, CS_MUTEX = 16, CS_SPIN = 20 };

void cs_init(uint32_t cs) {
    memset(P(cs), 0, 24);
    wr32(cs + 4, 0xffffffff);  // LockCount: -1 = unlocked
    wr32(cs + CS_MUTEX, sync_new());
}
static pthread_mutex_t *cs_mutex(uint32_t cs) { return &sync_get(rd32(cs + CS_MUTEX))->m; }
static void cs_acquired(CPU *c, uint32_t cs) {
    wr32(cs + CS_OWNER, rd32(c->fs_base + TEB_TID));
    wr32(cs + CS_RECURSION, rd32(cs + CS_RECURSION) + 1);
}

HOST_STDCALL(kernel32, InitializeCriticalSection, 4) { cs_init(ARG(0)); }
HOST_STDCALL(kernel32, InitializeCriticalSectionAndSpinCount, 8) { cs_init(ARG(0)); ret_i32(c, 1); }
HOST_STDCALL(kernel32, InitializeCriticalSectionEx, 12) { cs_init(ARG(0)); ret_i32(c, 1); }
HOST_STDCALL(kernel32, EnterCriticalSection, 4) {
    pthread_mutex_lock(cs_mutex(ARG(0)));
    cs_acquired(c, ARG(0));
}
HOST_STDCALL(kernel32, TryEnterCriticalSection, 4) {
    int ok = !pthread_mutex_trylock(cs_mutex(ARG(0)));
    if (ok) cs_acquired(c, ARG(0));
    ret_i32(c, ok);
}
HOST_STDCALL(kernel32, LeaveCriticalSection, 4) {
    uint32_t cs = ARG(0), n = rd32(cs + CS_RECURSION) - 1;
    wr32(cs + CS_RECURSION, n);
    if (!n) wr32(cs + CS_OWNER, 0);
    pthread_mutex_unlock(cs_mutex(cs));
}
HOST_STDCALL(kernel32, DeleteCriticalSection, 4) {
    sync_free(rd32(ARG(0) + CS_MUTEX));
    wr32(ARG(0) + CS_MUTEX, 0);
}
HOST_STDCALL(kernel32, SetCriticalSectionSpinCount, 8) {
    uint32_t old = rd32(ARG(0) + CS_SPIN);
    wr32(ARG(0) + CS_SPIN, ARG(1));
    ret_i32(c, old);
}

// DllMain is never called for thread attach/detach anyway.
HOST_STDCALL(kernel32, DisableThreadLibraryCalls, 4) { ret_i32(c, 1); }

// Current directory, as a Z: path (hle.h).
static int cwd_win(char *out, size_t n) {
    char host[4096];
    return getcwd(host, sizeof host) && win_path(host, out, n);
}
HOST_STDCALL(kernel32, GetCurrentDirectoryW, 8) {  // (buffer length in wchars, buffer)
    char w[4096];
    if (!cwd_win(w, sizeof w)) return ret_i32(c, 0);
    ret_i32(c, utf8_to_utf16(w, ARG(1), ARG(0)));
}
HOST_STDCALL(kernel32, GetCurrentDirectoryA, 8) {
    char w[4096];
    if (!cwd_win(w, sizeof w)) return ret_i32(c, 0);
    uint32_t n = strlen(w);
    if (n + 1 > ARG(0)) return ret_i32(c, n + 1);
    memcpy(ARG_PTR(1), w, n + 1);
    ret_i32(c, n);
}
