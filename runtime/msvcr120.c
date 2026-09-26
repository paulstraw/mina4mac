// MSVCR120 implemented natively (HLE). Memory allocation is backed by the guest heap (heap.c).
#include "msvcr120.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "heap.h"
#include "hle.h"
#include "host.h"
#include "kernel32.h"
#include "proc.h"
#include "undname.h"

HOST_CDECL(msvcr120, malloc) { ret_i32(c, heap_alloc(ARG(0))); }
HOST_CDECL(msvcr120, _malloc_crt) { ret_i32(c, heap_alloc(ARG(0))); }
HOST_CDECL(msvcr120, calloc) { ret_i32(c, heap_calloc(ARG(0), ARG(1))); }
HOST_CDECL(msvcr120, _calloc_crt) { ret_i32(c, heap_calloc(ARG(0), ARG(1))); }
HOST_CDECL(msvcr120, free) { heap_free(ARG(0)); }
HOST_CDECL(msvcr120, _msize) { ret_i32(c, heap_size(ARG(0))); }

static uint32_t crt_realloc(uint32_t p, uint32_t size) {  // realloc(p, 0) frees p and returns NULL
    if (p && !size) { heap_free(p); return 0; }
    return heap_realloc(p, size);
}
HOST_CDECL(msvcr120, realloc) { ret_i32(c, crt_realloc(ARG(0), ARG(1))); }
HOST_CDECL(msvcr120, _realloc_crt) { ret_i32(c, crt_realloc(ARG(0), ARG(1))); }

HOST_CDECL(msvcr120, _aligned_malloc) { ret_i32(c, heap_aligned_alloc(ARG(0), ARG(1))); }
HOST_CDECL(msvcr120, _aligned_free) { heap_aligned_free(ARG(0)); }

// operator new would throw std::bad_alloc; with no guest exceptions yet, running out is fatal.
static uint32_t op_new(uint32_t size) {
    uint32_t p = heap_alloc(size);
    if (!p) { fprintf(stderr, "operator new(%u): out of guest heap\n", size); exit(6); }
    return p;
}
HOST(msvcr120, op_new, "??2@YAPAXI@Z", 0) { ret_i32(c, op_new(ARG(0))); }
HOST(msvcr120, op_delete, "??3@YAXPAX@Z", 0) { heap_free(ARG(0)); }
HOST(msvcr120, concrt_alloc, "?Alloc@Concurrency@@YAPAXI@Z", 0) { ret_i32(c, op_new(ARG(0))); }
HOST(msvcr120, concrt_free, "?Free@Concurrency@@YAXPAX@Z", 0) { heap_free(ARG(0)); }

// CRT startup (crtexe.c/crt0dat.c): the command line, the initializer tables and the exit path.
// Data imports (_acmdln, _fmode, _commode) live in their thunk's 16 bytes of guest memory: the IAT slot
// holds the thunk address, which guest code dereferences as the variable's address.
#define MSVCR "MSVCR120.dll"
static uint32_t ARGC, ARGV, ENVP;

void crt_init(int argc, char **argv) {
    // argv[0] is this launcher; the game sees itself as noita.exe.
    size_t len = 12;
    for (int i = 1; i < argc; i++) len += strlen(argv[i]) + 3;
    char *cmdline = malloc(len);
    strcpy(cmdline, "\"noita.exe\"");
    ARGC = argc;
    ARGV = heap_calloc(argc + 1, 4);
    wr32(ARGV, guest_strdup("noita.exe"));
    for (int i = 1; i < argc; i++) {  // quoted when needed; embedded quotes are not escaped
        int q = argv[i][0] == 0 || strpbrk(argv[i], " \t");
        strcat(cmdline, q ? " \"" : " ");
        strcat(cmdline, argv[i]);
        if (q) strcat(cmdline, "\"");
        wr32(ARGV + 4 * i, guest_strdup(argv[i]));
    }
    ENVP = heap_calloc(1, 4);  // an empty environment
    wr32(rt_thunk(MSVCR, "_acmdln"), guest_strdup(cmdline));
    free(cmdline);
    wr32(rt_thunk(MSVCR, "_fmode"), 0);    // _O_TEXT
    wr32(rt_thunk(MSVCR, "_commode"), 0);
}

// (int *argc, char ***argv, char ***envp, int dowildcard, _startupinfo *)
HOST_CDECL(msvcr120, __getmainargs) {
    wr32(ARG(0), ARGC);
    wr32(ARG(1), ARGV);
    wr32(ARG(2), ENVP);
    ret_i32(c, 0);
}

HOST_CDECL(msvcr120, __set_app_type) {}
HOST_CDECL(msvcr120, __setusermatherr) {}  // math errors are not reported to the guest
HOST_CDECL(msvcr120, __crtSetUnhandledExceptionFilter) {}
HOST_CDECL(msvcr120, __crtGetShowWindowMode) { ret_i32(c, 10); }  // SW_SHOWDEFAULT
HOST_CDECL(msvcr120, _configthreadlocale) { ret_i32(c, 2); }  // previous mode: _DISABLE_PER_THREAD_LOCALE
HOST_CDECL(msvcr120, _ismbblead) { ret_i32(c, 0); }  // the "C" locale's code page has no lead bytes

// _initterm(begin, end): call each non-null void (*)(void) in [begin, end).
HOST_CDECL(msvcr120, _initterm) {
    for (uint32_t p = ARG(0); p < ARG(1); p += 4)
        if (rd32(p)) call_guest(c, rd32(p), 0, NULL);
}
// _initterm_e(begin, end): the same with int (*)(void), stopping at the first non-zero result.
HOST_CDECL(msvcr120, _initterm_e) {
    uint32_t r = 0;
    for (uint32_t p = ARG(0); p < ARG(1) && !r; p += 4)
        if (rd32(p)) r = call_guest(c, rd32(p), 0, NULL);
    ret_i32(c, r);
}

// _controlfp_s(unsigned *current, unsigned value, unsigned mask). The CRT's abstract control word maps
// onto the x87 control word: exception masks, precision (_MCW_PC) and rounding (_MCW_RC). The runtime
// models x87 arithmetic in double precision and SSE with host defaults, so only the rounding mode takes
// effect; the rest is kept so it reads back. Denormal control (_MCW_DN) is not kept.
enum { MCW_EM = 0x8001f, MCW_RC = 0x300, MCW_PC = 0x30000 };
static const struct { uint32_t crt, x87; } FP_BITS[] = {
    {0x10, 0x01}, {0x80000, 0x02}, {0x08, 0x04}, {0x04, 0x08}, {0x02, 0x10}, {0x01, 0x20},  // masks
    {0x100, 0x400}, {0x200, 0x800},  // _RC_DOWN, _RC_UP (both: _RC_CHOP)
};
static uint32_t crt_cw(uint16_t cw) {
    uint32_t r = 0;
    for (size_t i = 0; i < sizeof FP_BITS / sizeof *FP_BITS; i++) if (cw & FP_BITS[i].x87) r |= FP_BITS[i].crt;
    int pc = (cw >> 8) & 3;
    return r | (pc == 0 ? 0x20000 : pc == 2 ? 0x10000 : 0);  // _PC_24, _PC_53, _PC_64 (0)
}
static uint16_t x87_cw(uint32_t crt, uint16_t cw) {
    cw &= ~0x0f3f;
    for (size_t i = 0; i < sizeof FP_BITS / sizeof *FP_BITS; i++) if (crt & FP_BITS[i].crt) cw |= FP_BITS[i].x87;
    uint32_t pc = crt & MCW_PC;
    return cw | (pc == 0x20000 ? 0 : pc == 0x10000 ? 0x200 : 0x300);
}
HOST_CDECL(msvcr120, _controlfp_s) {
    uint32_t mask = ARG(2) & (MCW_EM | MCW_RC | MCW_PC);
    if (mask) c->fpu_cw = x87_cw((crt_cw(c->fpu_cw) & ~mask) | (ARG(1) & mask), c->fpu_cw);
    if (ARG(0)) wr32(ARG(0), crt_cw(c->fpu_cw));
    ret_i32(c, 0);
}

// CRT locks (_lock(n)/_unlock(n)), recursive like Windows critical sections.
enum { NLOCKS = 64 };
static pthread_mutex_t LOCKS[NLOCKS];
__attribute__((constructor)) static void init_locks(void) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    for (int i = 0; i < NLOCKS; i++) pthread_mutex_init(&LOCKS[i], &a);
}
static pthread_mutex_t *crt_lock(uint32_t n) {
    if (n >= NLOCKS) { fprintf(stderr, "_lock(%u): no such CRT lock\n", n); exit(6); }
    return &LOCKS[n];
}
HOST_CDECL(msvcr120, _lock) { pthread_mutex_lock(crt_lock(ARG(0))); }
HOST_CDECL(msvcr120, _unlock) { pthread_mutex_unlock(crt_lock(ARG(0))); }

// Exit handlers: the CRT's own table (_onexit, which the exe's atexit uses), run by exit/_cexit, last
// registered first.
enum { MAX_ONEXIT = 1024 };
static uint32_t ONEXIT[MAX_ONEXIT];
static int NONEXIT;
static pthread_mutex_t ONEXIT_LOCK = PTHREAD_MUTEX_INITIALIZER;

HOST_CDECL(msvcr120, _onexit) {
    pthread_mutex_lock(&ONEXIT_LOCK);
    uint32_t f = NONEXIT < MAX_ONEXIT ? (ONEXIT[NONEXIT++] = ARG(0)) : 0;
    pthread_mutex_unlock(&ONEXIT_LOCK);
    ret_i32(c, f);
}

static void run_onexit(CPU *c) {
    for (;;) {
        pthread_mutex_lock(&ONEXIT_LOCK);
        uint32_t f = NONEXIT ? ONEXIT[--NONEXIT] : 0;
        pthread_mutex_unlock(&ONEXIT_LOCK);
        if (!f) break;
        call_guest(c, f, 0, NULL);
    }
}
HOST_CDECL(msvcr120, _cexit) { run_onexit(c); }
HOST_CDECL(msvcr120, exit) {
    run_onexit(c);
    fflush(NULL);
    exit((int)ARG(0));
}
HOST_CDECL(msvcr120, _exit) {
    fflush(NULL);
    _Exit((int)ARG(0));
}
HOST_CDECL(msvcr120, _amsg_exit) {  // fatal CRT runtime error _RT_<n>
    fprintf(stderr, "guest runtime error R60%02u\n", ARG(0));
    _Exit(255);
}

// __dllonexit(func, &begin, &end): append func to a DLL's own exit table, a guest heap block whose
// (encoded) bounds the DLL keeps; its DllMain runs the table on DLL_PROCESS_DETACH.
HOST_CDECL(msvcr120, __dllonexit) {
    uint32_t f = ARG(0), pbegin = ARG(1), pend = ARG(2);
    pthread_mutex_lock(&ONEXIT_LOCK);
    uint32_t begin = rd32(pbegin), end = rd32(pend), used = end - begin;
    if (!begin || used + 4 > heap_size(begin)) {  // grow by doubling (at least 32 entries)
        uint32_t size = used < 64 ? 128 : 2 * used, nb = heap_realloc(begin, size);
        if (!nb) f = 0;
        else begin = nb, end = nb + used;
    }
    if (f) {
        wr32(end, f);
        wr32(pbegin, begin);
        wr32(pend, end + 4);
    }
    pthread_mutex_unlock(&ONEXIT_LOCK);
    ret_i32(c, f);
}

// (CRITICAL_SECTION *, spin count, flags): the CRT's wrapper for InitializeCriticalSectionEx.
HOST_CDECL(msvcr120, __crtInitializeCriticalSectionEx) { cs_init(ARG(0)); ret_i32(c, 1); }

// type_info (thiscall: `this` in ecx): { vftable, cached undecorated name, decorated name ".?AV..." }.
HOST(msvcr120, type_info_name, "?name@type_info@@QBEPBDPAU__type_info_node@@@Z", 4) {
    uint32_t ti = c->ecx, cached = rd32(ti + 4);
    if (!cached) {  // undecorated once, then kept for the life of the process
        const char *dec = (const char *)P(ti + 8);
        char *u = undname_type(dec);
        uint32_t g = guest_strdup(u ? u : dec + 1);
        free(u);
        uint32_t zero = 0;
        cached = __atomic_compare_exchange_n((uint32_t *)P(ti + 4), &zero, g, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)
                     ? g : (heap_free(g), zero);
    }
    ret_i32(c, cached);
}
HOST(msvcr120, type_info_eq, "??8type_info@@QBE_NABV0@@Z", 4) {
    ret_i32(c, !strcmp((char *)P(c->ecx + 9), (char *)P(ARG(0) + 9)));
}
HOST(msvcr120, type_info_ne, "??9type_info@@QBE_NABV0@@Z", 4) {
    ret_i32(c, !!strcmp((char *)P(c->ecx + 9), (char *)P(ARG(0) + 9)));
}
HOST(msvcr120, type_info_dtor, "??1type_info@@UAE@XZ", 0) {}  // cached names are never freed
HOST_CDECL(msvcr120, __clean_type_info_names_internal) {}

HOST_CDECL(msvcr120, _errno) { ret_i32(c, c->fs_base + TEB_CRT_ERRNO); }

// Time.
HOST_CDECL(msvcr120, _time64) {
    int64_t t = time(NULL);
    if (ARG(0)) wr64(ARG(0), (uint64_t)t);
    ret_i64(c, (uint64_t)t);
}
static void wr_tm(uint32_t g, const struct tm *t) {  // struct tm: nine ints, the same on both sides
    const int v[9] = {t->tm_sec, t->tm_min, t->tm_hour, t->tm_mday, t->tm_mon, t->tm_year,
                      t->tm_wday, t->tm_yday, t->tm_isdst};
    for (int i = 0; i < 9; i++) wr32(g + 4 * i, (uint32_t)v[i]);
}
static _Thread_local uint32_t TM;  // the per-thread buffer localtime returns
HOST_CDECL(msvcr120, _localtime64) {
    time_t t = (time_t)(int64_t)rd64(ARG(0));
    struct tm tm;
    if (!localtime_r(&t, &tm)) return ret_i32(c, 0);
    if (!TM) TM = heap_calloc(1, 36);
    wr_tm(TM, &tm);
    ret_i32(c, TM);
}

// Exceptions: none are ever in flight (guest C++ exceptions aren't supported yet).
HOST_CDECL(msvcr120, __uncaught_exception) { ret_i32(c, 0); }
