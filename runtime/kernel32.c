// KERNEL32 implemented natively (HLE). Every heap handle maps to the one guest heap (heap.c).
#include "kernel32.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
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
// SYSTEMTIME is 8 WORDs: year, month, day of week, day, hour, minute, second, milliseconds.
static void wr_systemtime(uint32_t st, const struct tm *tm, uint32_t ms) {
    uint16_t v[8] = {tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_wday, tm->tm_mday, tm->tm_hour, tm->tm_min,
                     tm->tm_sec, ms};
    for (int i = 0; i < 8; i++) wr16(st + 2 * i, v[i]);
}
HOST_STDCALL(kernel32, FileTimeToSystemTime, 8) {
    uint64_t ft = rd64(ARG(0));
    time_t secs = (time_t)(ft / 10000000) - FILETIME_UNIX_EPOCH_SECS;
    struct tm tm;
    if (ft >> 63 || !gmtime_r(&secs, &tm)) return ret_i32(c, 0);
    wr_systemtime(ARG(1), &tm, ft / 10000 % 1000);
    ret_i32(c, 1);
}
HOST_STDCALL(kernel32, GetLocalTime, 4) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    wr_systemtime(ARG(0), &tm, ts.tv_nsec / 1000000);
}
// Only the host's current time zone (a NULL TIME_ZONE_INFORMATION) is supported.
HOST_STDCALL(kernel32, SystemTimeToTzSpecificLocalTime, 12) {
    uint32_t u = ARG(1);
    struct tm tm = {.tm_year = rd16(u) - 1900, .tm_mon = rd16(u + 2) - 1, .tm_mday = rd16(u + 6),
                    .tm_hour = rd16(u + 8), .tm_min = rd16(u + 10), .tm_sec = rd16(u + 12)};
    time_t secs = timegm(&tm);
    if (ARG(0) || !localtime_r(&secs, &tm)) return ret_i32(c, 0);
    wr_systemtime(ARG(2), &tm, rd16(u + 14));
    ret_i32(c, 1);
}
HOST_STDCALL(kernel32, QueryPerformanceCounter, 4) {
    wr64(ARG(0), clock_gettime_nsec_np(CLOCK_UPTIME_RAW));
    ret_i32(c, 1);
}
HOST_STDCALL(kernel32, QueryPerformanceFrequency, 4) {
    wr64(ARG(0), 1000000000);
    ret_i32(c, 1);
}
// WINMM: the host timer resolution needs no raising; TIMERR_NOERROR.
HOST_STDCALL(winmm, timeBeginPeriod, 4) { ret_i32(c, 0); }

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

// Handles (kernel32.h): index n is handle 4 * (n + 1). Threads are named by guest thread id, processes by
// guest pid, find handles (kernel32_file.c) by a host pointer.
enum { MAX_HANDLES = 4096 };
static struct { enum HandleKind kind; uint64_t data; } HANDLES[MAX_HANDLES];
static pthread_mutex_t HANDLES_LOCK = PTHREAD_MUTEX_INITIALIZER;

uint32_t handle_new(enum HandleKind kind, uint64_t data) {
    pthread_mutex_lock(&HANDLES_LOCK);
    uint32_t h = 0;
    for (uint32_t i = 0; i < MAX_HANDLES && !h; i++)
        if (HANDLES[i].kind == HK_FREE) HANDLES[i].kind = kind, HANDLES[i].data = data, h = 4 * (i + 1);
    pthread_mutex_unlock(&HANDLES_LOCK);
    return h;
}

int handle_get(uint32_t h, enum HandleKind kind, uint64_t *data) {
    uint32_t i = h / 4 - 1;
    if (h % 4 || i >= MAX_HANDLES) return 0;
    pthread_mutex_lock(&HANDLES_LOCK);
    int ok = HANDLES[i].kind == kind;
    if (ok && data) *data = HANDLES[i].data;
    pthread_mutex_unlock(&HANDLES_LOCK);
    return ok;
}

HOST_STDCALL(kernel32, GetCurrentProcess, 0) { ret_i32(c, H_CURRENT_PROCESS); }
HOST_STDCALL(kernel32, GetCurrentThread, 0) { ret_i32(c, H_CURRENT_THREAD); }

int handle_close(uint32_t h) {
    uint32_t i = h / 4 - 1;
    pthread_mutex_lock(&HANDLES_LOCK);
    int ok = h % 4 == 0 && i < MAX_HANDLES && HANDLES[i].kind != HK_FREE;
    if (ok) HANDLES[i].kind = HK_FREE;
    pthread_mutex_unlock(&HANDLES_LOCK);
    return ok;
}

HOST_STDCALL(kernel32, CloseHandle, 4) {
    uint32_t h = ARG(0);
    ret_i32(c, h == H_CURRENT_PROCESS || h == H_CURRENT_THREAD || handle_close(h));
}

// DuplicateHandle(src process, src handle, dst process, &dst handle, access, inherit, options). Only
// within this process, and only thread and process handles (including the pseudo-handles).
HOST_STDCALL(kernel32, DuplicateHandle, 28) {
    uint32_t src = ARG(1), out = ARG(3);
    uint64_t data;
    enum HandleKind kind = src == H_CURRENT_THREAD ? (data = rd32(c->fs_base + TEB_TID), HK_THREAD)
                         : src == H_CURRENT_PROCESS ? (data = GUEST_PID, HK_PROCESS)
                         : handle_get(src, HK_THREAD, &data) ? HK_THREAD
                         : handle_get(src, HK_PROCESS, &data) ? HK_PROCESS : HK_FREE;
    uint32_t h = kind == HK_FREE ? 0 : handle_new(kind, data);
    if (out) wr32(out, h);
    ret_i32(c, h != 0);
}

// SYSTEM_INFO for an x86 (Pentium Pro family) machine with the host's processor count (at most 32, the
// width of the affinity mask).
HOST_STDCALL(kernel32, GetSystemInfo, 4) {
    uint32_t si = ARG(0);
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    n = n < 1 ? 1 : n > 32 ? 32 : n;
    memset(P(si), 0, 36);
    wr32(si + 4, 0x1000);                                     // dwPageSize
    wr32(si + 8, 0x10000);                                    // lpMinimumApplicationAddress
    wr32(si + 12, 0xfffeffff);                                // lpMaximumApplicationAddress (large address aware)
    wr32(si + 16, n == 32 ? 0xffffffff : (1u << n) - 1);      // dwActiveProcessorMask
    wr32(si + 20, n);                                         // dwNumberOfProcessors
    wr32(si + 24, 586);                                       // dwProcessorType: PROCESSOR_INTEL_PENTIUM
    wr32(si + 28, 0x10000);                                   // dwAllocationGranularity
    wr16(si + 32, 6);                                         // wProcessorLevel
}

// GetVersionExA(OSVERSIONINFOA or OSVERSIONINFOEXA): the version in the PEB (proc.c), Windows 7 SP1.
HOST_STDCALL(kernel32, GetVersionExA, 4) {
    uint32_t vi = ARG(0), size = rd32(vi);
    if (size != 148 && size != 156) return ret_i32(c, 0);
    memset(P(vi + 4), 0, size - 4);
    wr32(vi + 4, rd32(PEB_ADDR + PEB_OS_MAJOR));
    wr32(vi + 8, rd32(PEB_ADDR + PEB_OS_MINOR));
    wr32(vi + 12, rd16(PEB_ADDR + PEB_OS_BUILD));
    wr32(vi + 16, rd32(PEB_ADDR + PEB_OS_PLATFORM));
    strcpy((char *)P(vi + 20), "Service Pack 1");
    if (size == 156) wr16(vi + 148, 1), wr8(vi + 154, 1);     // wServicePackMajor, wProductType: workstation
    ret_i32(c, 1);
}

// Dynamic loading. A recompiled module (rt_register_module) is its own handle; the DLLs the runtime
// stands in for (host implementations) get fake handles in HLE_MODULES_LO + 0x10000 * n, with nothing
// mapped there. GetProcAddress on those returns the dll!name thunk for any name, like import binding:
// calling one with no host implementation exits naming it. Other DLLs fail to load.
enum { HLE_MODULES_LO = 0xE0000000u, ERROR_MOD_NOT_FOUND = 126, ERROR_PROC_NOT_FOUND = 127 };
static const char *const HLE_DLLS[] = {
    "kernel32.dll", "user32.dll", "gdi32.dll", "opengl32.dll", "shell32.dll", "shlwapi.dll", "ole32.dll",
    "winmm.dll", "wininet.dll", "ws2_32.dll", "comdlg32.dll", "msvcr120.dll",
    "SDL2.dll", "lua51.dll", "fmod.dll", "fmodstudio.dll", "Galaxy.dll",
};
enum { NHLE_DLLS = sizeof HLE_DLLS / sizeof *HLE_DLLS };

HOST_STDCALL(kernel32, LoadLibraryA, 4) {
    const char *name = ARG_STR(0), *slash = strrchr(name, '\\');
    char dll[256];
    snprintf(dll, sizeof dll, "%s%s", slash ? slash + 1 : name, strchr(slash ? slash : name, '.') ? "" : ".dll");
    uint32_t base = rt_module_base(dll);
    for (uint32_t i = 0; i < NHLE_DLLS && !base; i++)
        if (!strcasecmp(HLE_DLLS[i], dll)) base = HLE_MODULES_LO + 0x10000 * i;
    if (!base) wr32(c->fs_base + TEB_LAST_ERROR, ERROR_MOD_NOT_FOUND);
    ret_i32(c, base);
}

HOST_STDCALL(kernel32, FreeLibrary, 4) { ret_i32(c, 1); }  // modules stay loaded

// GetProcAddress(module, name or ordinal).
HOST_STDCALL(kernel32, GetProcAddress, 8) {
    uint32_t mod = ARG(0), sym = ARG(1);
    char name[512];
    if (sym < 0x10000) snprintf(name, sizeof name, "#%u", sym);
    else snprintf(name, sizeof name, "%s", (const char *)P(sym));
    uint32_t i = (mod - HLE_MODULES_LO) / 0x10000, f;
    if (mod >= HLE_MODULES_LO && i < NHLE_DLLS && mod % 0x10000 == 0) f = rt_thunk(HLE_DLLS[i], name);
    else f = mod ? rt_export(mod, name) : 0;
    if (!f) wr32(c->fs_base + TEB_LAST_ERROR, ERROR_PROC_NOT_FOUND);
    ret_i32(c, f);
}

// Virtual memory. Guest memory is always mapped read/write, so VirtualAlloc hands out zeroed,
// 64 KB-aligned blocks of the guest heap (reserve and commit are the same), and protections are only
// recorded as far as callers can observe them. The game's code is recompiled, so writes to its code
// (VirtualProtect then patch, as noita.exe does to LuaJIT functions for its mod sandbox) have no effect
// on execution; the trace notes them.
enum { MEM_COMMIT = 0x1000, MEM_RESERVE = 0x2000, MEM_DECOMMIT = 0x4000, MEM_RELEASE = 0x8000 };
enum { PAGE_READWRITE = 0x04, PAGE_EXECUTE_READ = 0x20, ERROR_INVALID_ADDRESS = 487 };
enum { MAX_REGIONS = 4096 };
static struct { uint32_t base, size; } REGIONS[MAX_REGIONS];
static pthread_mutex_t REGIONS_LOCK = PTHREAD_MUTEX_INITIALIZER;

static int region_of(uint32_t a) {  // lock held; -1 if none
    for (int i = 0; i < MAX_REGIONS; i++)
        if (REGIONS[i].size && a - REGIONS[i].base < REGIONS[i].size) return i;
    return -1;
}

HOST_STDCALL(kernel32, VirtualAlloc, 16) {  // (address, size, allocation type, protection)
    uint32_t addr = ARG(0), size = (ARG(1) + 0xfff) & ~0xfffu, r = 0;
    pthread_mutex_lock(&REGIONS_LOCK);
    if (addr) {  // committing (part of) a region reserved earlier
        int i = region_of(addr);
        if (i >= 0 && addr - REGIONS[i].base + size <= REGIONS[i].size) r = addr & ~0xfffu;
    } else if (size && (r = heap_aligned_alloc(size, 0x10000))) {
        int i = 0;
        while (i < MAX_REGIONS && REGIONS[i].size) i++;
        if (i == MAX_REGIONS) heap_aligned_free(r), r = 0;
        else memset(P(r), 0, size), REGIONS[i].base = r, REGIONS[i].size = size;
    }
    pthread_mutex_unlock(&REGIONS_LOCK);
    if (!r) wr32(c->fs_base + TEB_LAST_ERROR, ERROR_INVALID_ADDRESS);
    ret_i32(c, r);
}

HOST_STDCALL(kernel32, VirtualFree, 12) {  // (address, size, free type)
    uint32_t addr = ARG(0), type = ARG(2);
    pthread_mutex_lock(&REGIONS_LOCK);
    int i = region_of(addr), ok = i >= 0 && (type == MEM_DECOMMIT || (type == MEM_RELEASE && REGIONS[i].base == addr));
    if (ok && type == MEM_RELEASE) heap_aligned_free(addr), REGIONS[i].size = 0;
    pthread_mutex_unlock(&REGIONS_LOCK);
    if (!ok) wr32(c->fs_base + TEB_LAST_ERROR, ERROR_INVALID_ADDRESS);
    ret_i32(c, ok);
}

// VirtualProtect(address, size, new protection, &old protection): the old protection reported is
// execute-read inside a mapped image's code, read-write elsewhere.
static int in_image_code(uint32_t a) { return rt_lookup(a) != NULL; }
HOST_STDCALL(kernel32, VirtualProtect, 16) {
    uint32_t addr = ARG(0), old = ARG(3);
    int code = in_image_code(addr) || (addr >= THUNK_BASE);
    if (old) wr32(old, code ? PAGE_EXECUTE_READ : PAGE_READWRITE);
    if (rt_trace && code) fprintf(stderr, "[noitamac] VirtualProtect(%#x, %#x) on code: patches won't take effect\n", addr, ARG(1));
    ret_i32(c, 1);
}

HOST_STDCALL(kernel32, FlushInstructionCache, 12) { ret_i32(c, 1); }
