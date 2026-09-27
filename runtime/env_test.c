// Guest process environment test (built and run by tools/check.sh): the guest heap directly, from many
// host threads, and through the MSVCR120/KERNEL32 imports; then the PEB, main-thread TEB, stack and
// static TLS set up for noita.exe; then the CRT startup imports (command line, initializer tables,
// exit handlers, _controlfp_s) and the KERNEL32 identity/time imports it calls; then the imports the C++
// static initializers use (memory/string/locale/stdio/printf/math/type_info/ConcRT/critical sections/
// __dllonexit/paths), binding the exe's MSVCP120 imports to the recompiled msvcp120.dll's exports, and the
// command line and SDL2 helpers SDL2main's WinMain uses; then the imports SDL_main uses up to its window.
//   env_test <noita image.bin> <msvcp120.dll>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "heap.h"
#include "hle.h"
#include "host.h"
#include "msvcr120.h"
#include "proc.h"

// "Recompiled" guest functions for the initializer tables and exit handlers: each appends its tag to
// CALLS and returns (cdecl, no args) 0, except G_SEVEN which returns 7.
enum { G_A = 0x300000, G_B = 0x300010, G_SEVEN = 0x300020 };
static char CALLS[16];
static void guest_log(CPU *c, char tag, uint32_t r) {
    CALLS[strlen(CALLS)] = tag;
    c->eax = r;
    c->esp += 4;
}
static void F_a(CPU *c) { guest_log(c, 'a', 0); }
static void F_b(CPU *c) { guest_log(c, 'b', 0); }
static void F_seven(CPU *c) { guest_log(c, '7', 7); }
// Thread start routines (unsigned __stdcall start(void *arg)): G_THREAD records its thread id and returns
// arg + 1; G_ENDTHREAD calls _endthreadex(3 * arg) instead of returning.
enum { G_THREAD = 0x300030, G_ENDTHREAD = 0x300040 };
static uint32_t THREAD_SEEN;
static void F_thread(CPU *c) {
    __atomic_store_n(&THREAD_SEEN, rd32(c->fs_base + TEB_TID), __ATOMIC_RELEASE);
    c->eax = rd32(c->esp + 4) + 1;
    c->esp += 8;
}
static void F_endthread(CPU *c) {
    uint32_t arg = rd32(c->esp + 4);
    c->esp -= 8;
    wr32(c->esp + 4, 3 * arg);
    wr32(c->esp, 0x0badf000);
    guest_call(c, rt_thunk("MSVCR120.dll", "_endthreadex"));
}
const FnEntry FN_TABLE[] = {{G_A, F_a}, {G_B, F_b}, {G_SEVEN, F_seven}, {G_THREAD, F_thread}, {G_ENDTHREAD, F_endthread}};
const int FN_COUNT = 5;

enum { EXE_BASE = 0x400000, RET_ADDR = 0x0badf000, THREADS = 8, ITERS = 200000, LIVE_MAX = 256 };

static int fails;
#define CHECK(name, got, want)                                                                        \
    do {                                                                                              \
        uint64_t g_ = (got), w_ = (want);                                                             \
        printf("%-32s %s got %#llx want %#llx\n", name, g_ == w_ ? "ok  " : "FAIL", g_, w_);         \
        fails += g_ != w_;                                                                            \
    } while (0)

static int in_heap(uint32_t p, uint32_t n) { return p >= HEAP_LO && p + n <= HEAP_HI && p + n >= p; }

// Each thread keeps up to LIVE_MAX blocks filled with a per-block byte and checks them on free, so any
// overlap between blocks (from a race or an allocator bug) shows up as corruption.
static void *hammer(void *arg) {
    uint32_t seed = (uint32_t)(uintptr_t)arg * 2654435761u + 1, live[LIVE_MAX] = {0}, len[LIVE_MAX];
    long bad = 0;
    for (int i = 0; i < ITERS; i++) {
        seed = seed * 1103515245 + 12345;
        int k = (seed >> 8) % LIVE_MAX;
        uint8_t tag = (uint8_t)((uintptr_t)arg * 31 + k);
        if (live[k]) {
            for (uint32_t j = 0; j < len[k]; j++) bad += rd8(live[k] + j) != tag;
            if ((seed >> 20) & 1) { heap_free(live[k]); live[k] = 0; continue; }
            uint32_t n = (seed >> 4) % 3000;
            live[k] = heap_realloc(live[k], n);
            if (n > len[k]) memset(P(live[k] + len[k]), tag, n - len[k]);
            len[k] = n;
        } else {
            uint32_t n = (seed >> 12) & 1 ? (seed >> 4) % 200 : (seed >> 4) % 70000;
            live[k] = heap_alloc(n);
            bad += (live[k] & 15) || !in_heap(live[k], n);
            memset(P(live[k]), tag, n);
            len[k] = n;
        }
    }
    for (int k = 0; k < LIVE_MAX; k++) heap_free(live[k]);
    return (void *)bad;
}

// Guest-side call of dll!name with 4-byte args; returns esp after the call (before any caller cleanup).
static uint32_t call(CPU *c, const char *dll, const char *name, int n, const uint32_t *args) {
    uint32_t top = c->esp;
    for (int i = n - 1; i >= 0; i--) { c->esp -= 4; wr32(c->esp, args[i]); }
    c->esp -= 4;
    wr32(c->esp, RET_ADDR);
    guest_call(c, rt_thunk(dll, name));
    uint32_t sp = c->esp;
    c->esp = top;
    return sp;
}
#define CRT(name, ...) (call(&c, "MSVCR120.dll", name, sizeof((uint32_t[]){__VA_ARGS__}) / 4, (uint32_t[]){__VA_ARGS__}), c.eax)
#define SDL(name, ...) (call(&c, "SDL2.dll", name, sizeof((uint32_t[]){__VA_ARGS__}) / 4, (uint32_t[]){__VA_ARGS__}), c.eax)
#define K32(name, ...) (call(&c, "KERNEL32.dll", name, sizeof((uint32_t[]){__VA_ARGS__}) / 4, (uint32_t[]){__VA_ARGS__}), c.eax)

static int all(uint32_t p, uint8_t v, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) if (rd8(p + i) != v) return 0;
    return 1;
}

static uint32_t gs(const char *s) { return guest_strdup(s); }
static uint32_t gws(const char *s) {
    uint32_t p = heap_alloc(2 * (strlen(s) + 1));
    utf8_to_utf16(s, p, strlen(s) + 1);
    return p;
}
static int gstreq(uint32_t p, const char *s) { return p && !strcmp((char *)P(p), s); }

// A thiscall method: `this` in ecx.
static uint32_t method(CPU *c, uint32_t this, const char *name, int n, const uint32_t *args) {
    c->ecx = this;
    call(c, "MSVCR120.dll", name, n, args);
    return c->eax;
}

#define ST_OF(c, i) (c).st[((c).st_top + (i)) & 7]

// Try to take a lock from another host thread (as guest thread c2), releasing it again on success.
typedef struct { CPU *c; uint32_t obj; int concrt; uint32_t r; } TryArgs;
static void *try_other_thread(void *p) {
    TryArgs *a = p;
    if (a->concrt) {
        a->r = method(a->c, a->obj, "?try_lock@critical_section@Concurrency@@QAE_NXZ", 0, NULL);
        if (a->r) method(a->c, a->obj, "?unlock@critical_section@Concurrency@@QAEXXZ", 0, NULL);
    } else {
        call(a->c, "KERNEL32.dll", "TryEnterCriticalSection", 1, &a->obj);
        a->r = a->c->eax;
        if (a->r) call(a->c, "KERNEL32.dll", "LeaveCriticalSection", 1, &a->obj);
    }
    return NULL;
}
static uint32_t on_other_thread(CPU *c2, uint32_t obj, int concrt) {
    TryArgs a = {c2, obj, concrt, 0};
    pthread_t t;
    pthread_create(&t, NULL, try_other_thread, &a);
    pthread_join(t, NULL);
    return a.r;
}

static void initializer_imports(CPU *cp, CPU *c2) {
    CPU c = *cp;  // the CRT/K32 macros use a local `c`
    // Memory, strings, conversions.
    uint32_t s1 = gs("hello world"), buf = heap_calloc(1, 256), endp = heap_alloc(4);
    CHECK("memcpy returns dest", CRT("memcpy", buf, s1, 12), buf);
    CHECK("strlen/strcmp", CRT("strlen", buf) * 10 + (CRT("strcmp", buf, s1) == 0), 111);
    CHECK("memchr", CRT("memchr", s1, 'w', 11), s1 + 6);
    uint32_t num = gs("  -123abc");
    CHECK("strtol value", CRT("strtol", num, endp, 10), (uint32_t)-123);
    CHECK("strtol endptr", rd32(endp), num + 6);
    CHECK("strtoul hex", CRT("strtoul", gs("0xffffffff"), 0, 16), 0xffffffff);
    int depth = c.st_top;
    CRT("strtod", gs("2.5e3"), 0);
    CHECK("strtod in st0", c.st_top == ((depth - 1) & 7) && ST_OF(c, 0) == 2500.0, 1);
    c.st_top = depth;
    CRT("srand", 1);
    uint32_t r1 = CRT("rand"), r2 = CRT("rand"), r3 = CRT("rand");
    CHECK("rand: MSVC LCG sequence", r1 == 41 && r2 == 18467 && r3 == 6334, 1);
    uint32_t wbuf = heap_alloc(32), conv = heap_alloc(4);
    CHECK("mbstowcs_s", CRT("mbstowcs_s", conv, wbuf, 16, s1, 0xffffffff), 0);
    CHECK("... converted incl. NUL, wide", rd32(conv) == 12 && rd16(wbuf) == 'h' && rd16(wbuf + 22) == 0, 1);
    CHECK("mbstowcs_s too small", CRT("mbstowcs_s", conv, wbuf, 4, s1, 11) == 34 && rd16(wbuf) == 0, 1);
    CHECK("mbstowcs_s _TRUNCATE", CRT("mbstowcs_s", conv, wbuf, 4, s1, 0xffffffff) == 80 && rd32(conv) == 4, 1);
    uint32_t ep = heap_alloc(8), ep2 = heap_alloc(8);
    wr64(ep, ~0ull);
    CRT("?__ExceptionPtrCreate@@YAXPAX@Z", ep);
    CRT("?__ExceptionPtrCopy@@YAXPAXPBX@Z", ep2, ep);
    CHECK("exception_ptr: null, copies null", rd64(ep) == 0 && rd64(ep2) == 0
          && CRT("?__ExceptionPtrToBool@@YA_NPBX@Z", ep) == 0, 1);
    CHECK("strcpy_s too small", CRT("strcpy_s", buf, 4, s1) == 34 && rd8(buf) == 0, 1);

    // Locale.
    CHECK("setlocale query = C", gstreq(CRT("setlocale", 0, 0), "C"), 1);
    CHECK("setlocale other fails", CRT("setlocale", 0, gs("German")), 0);
    CHECK("localeconv decimal point", gstreq(rd32(CRT("localeconv")), "."), 1);
    uint32_t pct = CRT("__pctype_func");
    CHECK("_pctype a/7/space/EOF", rd16(pct + 2 * 'a') == 0x182 && rd16(pct + 2 * '7') == 0x84
          && rd16(pct + 2 * ' ') == 0x48 && rd16(pct - 2) == 0, 1);
    CHECK("isdigit/isspace/toupper", (CRT("isdigit", '5') != 0) + (CRT("isspace", 'x') != 0) * 2
          + (CRT("toupper", 'q') == 'Q') * 4 + (CRT("tolower", 0xc4) == 0xc4) * 8, 13);

    // printf formatting (guest varargs; doubles and __int64 take two slots).
    double pi = 3.14159265, big = 1500.0, huge = 1e20;
    int64_t i64 = -5000000000ll;
    uint32_t w = gws("wide"), ab = gs("ab"), fmtp = gs("%d|%5s|%-4x|%08.3f|%e|%I64d|%p|%S|%c|%%|%g|%s");
    uint32_t args[] = {buf, 256, fmtp, (uint32_t)-12, ab, 0xff, 0, 0, 0, 0, 0, 0, 0xabcd, w, 'Z', 0, 0, 0};
    memcpy(&args[6], &pi, 8);
    memcpy(&args[8], &big, 8);
    memcpy(&args[10], &i64, 8);
    memcpy(&args[15], &huge, 8);
    uint32_t n = (call(&c, "MSVCR120.dll", "sprintf_s", 18, args), c.eax);
    const char *want = "-12|   ab|ff  |0003.142|1.500000e+003|-5000000000|0000ABCD|wide|Z|%|1e+020|(null)";
    CHECK("sprintf_s MSVC format", strcmp((char *)P(buf), want), 0);
    if (strcmp((char *)P(buf), want)) printf("  got \"%s\"\n", (char *)P(buf));
    CHECK("sprintf_s length", n, strlen(want));
    wr32(endp, s1);  // a va_list holding one char *
    CHECK("_vsnprintf truncates: -1", CRT("_vsnprintf", buf, 4, gs("%s"), endp), 0xffffffff);
    CHECK("_vsnprintf truncated bytes", memcmp(P(buf), "hell", 4), 0);
    CHECK("sprintf_s overflow", CRT("sprintf_s", buf, 4, gs("%s"), s1) == 0xffffffff && rd8(buf) == 0, 1);

    // stdio: a file round trip through a backslash path, and errno for a missing file.
    uint32_t name = gs("build\\tmp\\envtest.txt"), f = CRT("fopen", name, gs("wt"));
    CHECK("fopen write", f != 0, 1);
    CRT("fputs", gs("line1\n"), f);
    CRT("fprintf", f, gs("%d-%s"), 42, ab);
    CRT("fclose", f);
    f = CRT("_fsopen", name, gs("rb"), 0x40);
    uint32_t got = heap_calloc(1, 64);
    for (int i = 0; i < 11; i++) wr8(got + i, (uint8_t)CRT("fgetc", f));
    CHECK("file contents", strcmp((char *)P(got), "line1\n42-ab"), 0);
    CHECK("fgetc at EOF", CRT("fgetc", f), 0xffffffff);
    uint32_t pos = heap_alloc(8);
    CRT("fgetpos", f, pos);
    CHECK("fgetpos", rd64(pos), 11);
    CRT("fseek", f, 2, 0);
    CHECK("fseek + fgetwc (binary: 2 bytes)", CRT("fgetwc", f), 'e' << 8 | 'n');
    CRT("fclose", f);
    CHECK("fopen missing: NULL", CRT("fopen", gs("no\\such\\file"), gs("r")), 0);
    CHECK("... errno ENOENT", rd32(CRT("_errno")), 2);
    uint32_t iob = CRT("__iob_func");
    CHECK("__iob_func: stdout/stderr work", CRT("fflush", iob + 32) == 0 && CRT("fflush", iob + 64) == 0, 1);

    // Math.
    depth = c.st_top;
    st_push(&c, 7.5);
    st_push(&c, 2.0);
    CRT("_CIfmod");
    CHECK("_CIfmod(st1, st0)", c.st_top == ((depth - 1) & 7) && ST_OF(c, 0) == 1.5, 1);
    c.st_top = depth;
    c.xmm[0].f64[0] = 2, c.xmm[1].f64[0] = 10;
    CRT("_libm_sse2_pow_precise");
    CHECK("_libm_sse2_pow_precise", c.xmm[0].f64[0] == 1024.0, 1);
    uint32_t d = heap_alloc(8);
    wrf64(d, 1.0 / 0.0);
    CHECK("_dtest inf", CRT("_dtest", d), 1);

    // Time.
    uint32_t tt = heap_alloc(8);
    uint64_t now = (CRT("_time64", tt), (uint64_t)c.edx << 32 | c.eax);
    CHECK("_time64 after 2020, stored", now > 1577836800 && rd64(tt) == now, 1);
    uint32_t tm = CRT("_localtime64", tt);
    CHECK("_localtime64 year", rd32(tm + 20) >= 120 && rd32(tm + 16) < 12, 1);
    for (int i = 0; i < 9; i++) wr32(tm + 4 * i, (uint32_t[]){5, 4, 3, 2, 0, 125, 4, 1, 0}[i]);  // 2025-01-02 03:04:05
    uint32_t tbuf = heap_calloc(1, 32);
    CHECK("strftime", CRT("strftime", tbuf, 32, gs("%y%m%d-%H%M%S"), tm) == 13 && gstreq(tbuf, "250102-030405"), 1);
    CHECK("strftime too small", CRT("strftime", tbuf, 13, gs("%y%m%d-%H%M%S"), tm), 0);
    char hcwd[4096], wcwd_[4096];
    getcwd(hcwd, sizeof hcwd), win_path(hcwd, wcwd_, sizeof wcwd_);
    CHECK("_getcwd (Z:)", gstreq(CRT("_getcwd", 0, 0), wcwd_) && wcwd_[0] == 'Z', 1);
    CHECK("_getcwd too small", CRT("_getcwd", tbuf, 2) == 0 && rd32(CRT("_errno")) == 34, 1);

    // type_info: { vftable, cached name, ".?AV..." }.
    uint32_t ti = heap_calloc(1, 64), ti2 = heap_calloc(1, 64);
    strcpy((char *)P(ti + 8), ".?AV?$vector@HV?$allocator@H@std@@@std@@");
    strcpy((char *)P(ti2 + 8), ".?AUEaseIn@Back@easing@ceng@@");
    uint32_t nm = method(&c, ti, "?name@type_info@@QBEPBDPAU__type_info_node@@@Z", 1, (uint32_t[]){0});
    CHECK("type_info::name", gstreq(nm, "class std::vector<int,class std::allocator<int> >"), 1);
    CHECK("type_info::name cached", method(&c, ti, "?name@type_info@@QBEPBDPAU__type_info_node@@@Z", 1, (uint32_t[]){0}), nm);
    CHECK("type_info::name struct", gstreq(method(&c, ti2, "?name@type_info@@QBEPBDPAU__type_info_node@@@Z", 1,
                                                  (uint32_t[]){0}), "struct ceng::easing::Back::EaseIn"), 1);
    CHECK("type_info ==/!=", method(&c, ti, "??8type_info@@QBE_NABV0@@Z", 1, (uint32_t[]){ti}) * 2
          + method(&c, ti, "??9type_info@@QBE_NABV0@@Z", 1, (uint32_t[]){ti2}), 3);

    // KERNEL32 critical sections: recursive, owner/recursion fields, exclusive across threads.
    uint32_t cs = heap_calloc(1, 24);
    K32("InitializeCriticalSection", cs);
    K32("EnterCriticalSection", cs);
    K32("EnterCriticalSection", cs);
    CHECK("CS owner + recursion", rd32(cs + 12) == GUEST_PID + 4 && rd32(cs + 8) == 2, 1);
    CHECK("CS TryEnter from other thread", on_other_thread(c2, cs, 0), 0);
    K32("LeaveCriticalSection", cs);
    K32("LeaveCriticalSection", cs);
    CHECK("CS released", rd32(cs + 12) == 0 && on_other_thread(c2, cs, 0) == 1, 1);
    K32("DeleteCriticalSection", cs);
    uint32_t cs2 = heap_calloc(1, 24);
    CRT("__crtInitializeCriticalSectionEx", cs2, 4000, 0);
    CHECK("__crtInitializeCriticalSectionEx", K32("TryEnterCriticalSection", cs2), 1);

    // ConcRT: critical_section, event.
    uint32_t ccs = heap_calloc(1, 64), ev = heap_calloc(1, 64);
    method(&c, ccs, "??0critical_section@Concurrency@@QAE@XZ", 0, NULL);
    method(&c, ccs, "?lock@critical_section@Concurrency@@QAEXXZ", 0, NULL);
    CHECK("critical_section held elsewhere", on_other_thread(c2, ccs, 1), 0);
    method(&c, ccs, "?unlock@critical_section@Concurrency@@QAEXXZ", 0, NULL);
    CHECK("critical_section free", on_other_thread(c2, ccs, 1), 1);
    method(&c, ev, "??0event@Concurrency@@QAE@XZ", 0, NULL);
    CHECK("event wait(0) unset: timeout", method(&c, ev, "?wait@event@Concurrency@@QAEII@Z", 1, (uint32_t[]){0}), 0xffffffff);
    method(&c, ev, "?set@event@Concurrency@@QAEXXZ", 0, NULL);
    CHECK("event wait after set", method(&c, ev, "?wait@event@Concurrency@@QAEII@Z", 1, (uint32_t[]){0}), 0);

    // __dllonexit: a DLL's own table grows and keeps order.
    uint32_t pb = heap_calloc(1, 4), pe = heap_calloc(1, 4);
    int order = 1;
    for (uint32_t i = 1; i <= 40; i++) CRT("__dllonexit", 0x1000 * i, pb, pe);
    for (uint32_t i = 0; i < 40; i++) order &= rd32(rd32(pb) + 4 * i) == 0x1000 * (i + 1);
    CHECK("__dllonexit: 40 entries in order", order && rd32(pe) - rd32(pb) == 160, 1);

    // Paths.
    uint32_t path = heap_calloc(260, 2);
    utf8_to_utf16("Z:\\a\\b", path, 260);
    call(&c, "SHLWAPI.dll", "PathAppendW", 2, (uint32_t[]){path, gws("\\..\\c\\.\\d.txt")});
    char out[600];
    utf16_to_utf8(path, out, sizeof out);
    CHECK("PathAppendW + canonicalise", c.eax == 1 && !strcmp(out, "Z:\\a\\c\\d.txt"), 1);
    CHECK("GetCurrentDirectoryW too small", K32("GetCurrentDirectoryW", 2, path) > 3, 1);
    uint32_t len = K32("GetCurrentDirectoryW", 260, path);
    utf16_to_utf8(path, out, sizeof out);
    char cwd[600], wcwd[600];
    CHECK("GetCurrentDirectoryW = Z: + cwd", len == strlen(out) && win_path(getcwd(cwd, sizeof cwd), wcwd, sizeof wcwd)
          && !strcmp(out, wcwd), 1);
    *cp = c;
}

// Binding the exe's MSVCP120 imports to the recompiled DLL's exports (by name, and by ordinal).
// A guest string compared with a host one, UTF-16.
static int gwstreq(uint32_t w, const char *s) {
    char u[4096];
    return w && utf16_to_utf8(w, u, sizeof u) && !strcmp(u, s);
}

// The imports SDL_main uses on the way to its window: known folders, the file system, dynamic loading,
// system info, virtual memory, threads, RTTI casts, the offline Galaxy stub and GL without a context.
static void sdl_main_imports(CPU *cp) {
    CPU c = *cp;
    char tmp[] = "/tmp/mina4mac_envtest.XXXXXX", p[4096], w[4096];
    CHECK("mkdtemp", mkdtemp(tmp) != NULL, 1);
    setenv("HOME", tmp, 1);  // known folders live under $HOME

    // SHELL32 / ole32.
    static const uint8_t LOCAL_LOW[16] = {0xa4, 0xa1, 0x20, 0xa5, 0x80, 0x17, 0xf6, 0x4f,
                                          0xbd, 0x18, 0x16, 0x73, 0x43, 0xc5, 0xaf, 0x16};
    uint32_t guid = heap_alloc(16), out = heap_alloc(4);
    memcpy(P(guid), LOCAL_LOW, 16);
    uint32_t hr = (call(&c, "SHELL32.dll", "SHGetKnownFolderPath", 4, (uint32_t[]){guid, 0x8000, 0, out}), c.eax);
    snprintf(p, sizeof p, "%s/Library/Application Support/mina4mac/AppData/LocalLow", tmp);
    win_path(p, w, sizeof w);
    struct stat st;
    CHECK("SHGetKnownFolderPath LocalLow", hr == 0 && gwstreq(rd32(out), w) && !stat(p, &st) && S_ISDIR(st.st_mode), 1);
    call(&c, "ole32.dll", "CoTaskMemFree", 1, (uint32_t[]){rd32(out)});
    CHECK("CoTaskMemFree", heap_owns(rd32(out)), 0);
    wr8(guid, 0);
    CHECK("SHGetKnownFolderPath unknown", (call(&c, "SHELL32.dll", "SHGetKnownFolderPath", 4, (uint32_t[]){guid, 0, 0, out}),
                                           c.eax == 0x80070057 && rd32(out) == 0), 1);
    snprintf(p, sizeof p, "%s/d/e/f", tmp);
    win_path(p, w, sizeof w);
    uint32_t dw = gws(w);
    CHECK("SHCreateDirectoryExW nested", (call(&c, "SHELL32.dll", "SHCreateDirectoryExW", 3, (uint32_t[]){0, dw, 0}), c.eax), 0);
    CHECK("SHCreateDirectoryExW exists", (call(&c, "SHELL32.dll", "SHCreateDirectoryExW", 3, (uint32_t[]){0, dw, 0}), c.eax), 183);

    // KERNEL32 file system, on files x.txt (5 bytes), Y.TXT and z.dat in the temp dir.
    const char *files[] = {"x.txt", "Y.TXT", "z.dat"};
    for (int i = 0; i < 3; i++) {
        snprintf(p, sizeof p, "%s/%s", tmp, files[i]);
        FILE *f = fopen(p, "w");
        fputs(i ? "" : "hello", f);
        fclose(f);
    }
    snprintf(p, sizeof p, "%s/*.txt", tmp);
    win_path(p, w, sizeof w);
    uint32_t fd = heap_calloc(1, 592), fh = K32("FindFirstFileW", gws(w), fd);
    char names[64] = "";
    uint32_t size_x = 0, finds = 0;
    for (int ok = fh != 0xffffffff; ok; ok = K32("FindNextFileW", fh, fd), finds++) {
        char u[260];
        utf16_to_utf8(fd + 44, u, sizeof u);
        strcat(names, u), strcat(names, " ");
        if (!strcmp(u, "x.txt")) size_x = rd32(fd + 32);
    }
    CHECK("FindFirst/NextFileW *.txt (any case)", finds == 2 && strstr(names, "x.txt") && strstr(names, "Y.TXT"), 1);
    CHECK("... size, then ERROR_NO_MORE_FILES", size_x == 5 && K32("GetLastError") == 18, 1);
    CHECK("FindClose", K32("FindClose", fh) * 2 + K32("FindClose", fh), 2);
    snprintf(p, sizeof p, "%s/*", tmp);
    win_path(p, w, sizeof w);
    fh = K32("FindFirstFileW", gws(w), fd);
    for (finds = 0; fh != 0xffffffff && (finds == 0 || K32("FindNextFileW", fh, fd)); finds++) {}
    K32("FindClose", fh);
    CHECK("FindFirstFileW * (+ . .. d Library)", finds, 7);
    snprintf(p, sizeof p, "%s/*.none", tmp);
    win_path(p, w, sizeof w);
    CHECK("FindFirstFileW no match", K32("FindFirstFileW", gws(w), fd) == 0xffffffff && K32("GetLastError") == 2, 1);
    snprintf(p, sizeof p, "%s/X.TX?", tmp);  // _findfirst64i32: ModDoesFileExist's existence test
    win_path(p, w, sizeof w);
    uint32_t ffd = heap_calloc(1, 296);
    CHECK("_findfirst64i32 wildcard, any case", CRT("_findfirst64i32", gs(w), ffd) != 0xffffffff
          && gstreq(ffd + 36, "x.txt") && rd32(ffd + 32) == 5 && rd32(ffd) == 0 && rd64(ffd + 24) > 1577836800, 1);
    snprintf(p, sizeof p, "%s/d", tmp);
    win_path(p, w, sizeof w);
    CHECK("_findfirst64i32 dir", CRT("_findfirst64i32", gs(w), ffd) != 0xffffffff && rd32(ffd) == 0x10, 1);
    snprintf(p, sizeof p, "%s/nope.txt", tmp);
    win_path(p, w, sizeof w);
    CHECK("_findfirst64i32 missing", CRT("_findfirst64i32", gs(w), ffd) == 0xffffffff && rd32(CRT("_errno")) == 2, 1);
    win_path(tmp, w, sizeof w);
    CHECK("GetFileAttributesA dir", K32("GetFileAttributesA", gs(w)), 0x10);
    snprintf(p, sizeof p, "%s/x.txt", tmp);
    win_path(p, w, sizeof w);
    uint32_t xa = gs(w), xw = gws(w);
    CHECK("GetFileAttributesW file", K32("GetFileAttributesW", xw), 0x80);
    snprintf(p, sizeof p, "%s/copy.txt", tmp);
    win_path(p, w, sizeof w);
    uint32_t ca = gs(w), cw = gws(w);
    CHECK("CopyFileA", K32("CopyFileA", xa, ca, 1), 1);
    CHECK("CopyFileA fail if exists", K32("CopyFileA", xa, ca, 1) == 0 && K32("GetLastError") == 80, 1);
    CHECK("MoveFileExW onto existing fails", K32("MoveFileExW", cw, xw, 0) == 0 && K32("GetLastError") == 183, 1);
    CHECK("MoveFileExW replace", K32("MoveFileExW", cw, xw, 1) == 1 && K32("GetFileAttributesW", cw) == 0xffffffff, 1);
    CHECK("... missing: ERROR_FILE_NOT_FOUND", K32("GetLastError"), 2);
    CHECK("DeleteFileW", K32("DeleteFileW", xw) * 2 + K32("DeleteFileW", xw), 2);
    snprintf(p, sizeof p, "%s/newdir", tmp);
    win_path(p, w, sizeof w);
    CHECK("CreateDirectoryA", K32("CreateDirectoryA", gs(w), 0) * 2 + K32("CreateDirectoryA", gs(w), 0), 2);

    // Dynamic loading.
    uint32_t gl = K32("LoadLibraryA", gs("opengl32.dll")), u32 = K32("LoadLibraryA", gs("C:\\Windows\\USER32"));
    CHECK("LoadLibraryA HLE dlls", gl && u32 && gl != u32, 1);
    CHECK("GetProcAddress -> thunk", K32("GetProcAddress", gl, gs("glClear")), rt_thunk("opengl32.dll", "glClear"));
    CHECK("LoadLibraryA unknown", K32("LoadLibraryA", gs("nope.dll")) == 0 && K32("GetLastError") == 126, 1);
    CHECK("FreeLibrary", K32("FreeLibrary", gl), 1);

    // System info and virtual memory.
    uint32_t si = heap_alloc(36);
    K32("GetSystemInfo", si);
    CHECK("GetSystemInfo page size, cpus", rd32(si + 4) == 0x1000 && rd32(si + 20) >= 1 && rd32(si + 20) <= 32, 1);
    uint32_t vi = heap_alloc(148);
    wr32(vi, 148);
    CHECK("GetVersionExA 6.1.7601", K32("GetVersionExA", vi) && rd32(vi + 4) == 6 && rd32(vi + 8) == 1
          && rd32(vi + 12) == 7601, 1);
    uint32_t va = K32("VirtualAlloc", 0, 0x5000, 0x3000, 4);
    CHECK("VirtualAlloc: 64K aligned, zeroed", va && va % 0x10000 == 0 && all(va, 0, 0x5000), 1);
    CHECK("VirtualAlloc commit inside", K32("VirtualAlloc", va + 0x1234, 0x100, 0x1000, 4), va + 0x1000);
    CHECK("VirtualAlloc commit outside", K32("VirtualAlloc", va + 0x5000, 0x100, 0x1000, 4), 0);
    uint32_t old = heap_alloc(4);
    CHECK("VirtualProtect data", K32("VirtualProtect", va, 16, 0x40, old) == 1 && rd32(old) == 4, 1);
    CHECK("VirtualProtect thunk: exec-read", K32("VirtualProtect", rt_thunk("lua51.dll", "x"), 16, 0x40, old) && rd32(old) == 0x20, 1);
    CHECK("VirtualFree release", K32("VirtualFree", va, 0, 0x8000) * 2 + K32("VirtualFree", va, 0, 0x8000), 2);

    // Threads and handles.
    uint32_t tid = heap_alloc(4), code = heap_alloc(4);
    uint32_t th = CRT("_beginthreadex", 0, 0, G_THREAD, 41, 4 /* CREATE_SUSPENDED */, tid);
    usleep(20000);
    CHECK("_beginthreadex suspended", th && K32("GetExitCodeThread", th, code) && rd32(code) == 259
          && K32("WaitForSingleObject", th, 0) == 0x102 && !__atomic_load_n(&THREAD_SEEN, __ATOMIC_ACQUIRE), 1);
    CHECK("ResumeThread", K32("ResumeThread", th), 1);
    CHECK("WaitForSingleObject", K32("WaitForSingleObject", th, 0xffffffff), 0);
    CHECK("thread: own id, exit code", THREAD_SEEN == rd32(tid) && THREAD_SEEN != GUEST_PID + 4
          && K32("GetExitCodeThread", th, code) && rd32(code) == 42, 1);
    CHECK("CloseHandle thread", K32("CloseHandle", th) * 2 + K32("CloseHandle", th), 2);
    th = CRT("_beginthreadex", 0, 0, G_ENDTHREAD, 5, 0, 0);
    CHECK("_endthreadex", K32("WaitForSingleObject", th, 5000) == 0 && K32("GetExitCodeThread", th, code) && rd32(code) == 15, 1);
    K32("CloseHandle", th);
    uint32_t dup = heap_alloc(4);
    CHECK("DuplicateHandle current thread", K32("DuplicateHandle", 0xffffffff, 0xfffffffe, 0xffffffff, dup, 0, 0, 2), 1);
    CHECK("... is a thread handle", K32("GetExitCodeThread", rd32(dup), code) && rd32(code) == 259, 1);
    CHECK("CloseHandle pseudo-handles", K32("CloseHandle", rd32(dup)) + K32("CloseHandle", 0xfffffffe), 2);

    // RTTI. TypeDescriptors: {vftable, spare, ".?AV<name>@@"}. Class B : A (single inheritance), and
    // class C : A, B2 with the B2 subobject at +8 (multiple inheritance).
    uint32_t r = heap_calloc(1, 1024), tda = r, tdb = r + 32, tdc = r + 64, tdb2 = r + 96, tdx = r + 128;
    strcpy((char *)P(tda + 8), ".?AVA@@"), strcpy((char *)P(tdb + 8), ".?AVB@@");
    strcpy((char *)P(tdc + 8), ".?AVC@@"), strcpy((char *)P(tdb2 + 8), ".?AVB2@@"), strcpy((char *)P(tdx + 8), ".?AVX@@");
    uint32_t bcd = r + 160;  // BaseClassDescriptors, 28 bytes each: {type, contained, mdisp, pdisp, vdisp, attr, chd}
    uint32_t types[] = {tdb, tda, tdc, tda, tdb2}, mdisps[] = {0, 0, 0, 0, 8};
    for (int i = 0; i < 5; i++) wr32(bcd + 28 * i, types[i]), wr32(bcd + 28 * i + 8, mdisps[i]), wr32(bcd + 28 * i + 12, -1);
    uint32_t arr = r + 320, chd = r + 360, col = r + 400, obj = r + 512;  // chd: B (2 bases), then C (3 bases, MI)
    for (int i = 0; i < 5; i++) wr32(arr + 4 * i, bcd + 28 * i);
    wr32(chd + 8, 2), wr32(chd + 12, arr);
    wr32(chd + 16 + 4, 1), wr32(chd + 16 + 8, 3), wr32(chd + 16 + 12, arr + 8);
    // COLs {sig, offset, cd offset, type, chd} for B, C@0, C@8, each followed by its one-slot vftable.
    uint32_t cols[][3] = {{0, tdb, chd}, {0, tdc, chd + 16}, {8, tdc, chd + 16}};
    for (int i = 0; i < 3; i++) {
        uint32_t o = col + 32 * i;
        wr32(o + 4, cols[i][0]), wr32(o + 12, cols[i][1]), wr32(o + 16, cols[i][2]), wr32(o + 24, o);  // vftable[-1] = COL
    }
    uint32_t objb = obj, objc = obj + 16;
    wr32(objb, col + 28), wr32(objc, col + 32 + 28), wr32(objc + 8, col + 64 + 28);
    CHECK("dynamic_cast SI down", CRT("__RTDynamicCast", objb, 0, tda, tdb, 0), objb);
    CHECK("dynamic_cast SI unrelated", CRT("__RTDynamicCast", objb, 0, tda, tdx, 0), 0);
    CHECK("dynamic_cast NULL", CRT("__RTDynamicCast", 0, 0, tda, tdb, 0), 0);
    CHECK("dynamic_cast MI B2 -> C", CRT("__RTDynamicCast", objc + 8, 0, tdb2, tdc, 0), objc);
    CHECK("dynamic_cast MI A -> B2 (cross)", CRT("__RTDynamicCast", objc, 0, tda, tdb2, 0), objc + 8);
    CHECK("__RTtypeid via B2 subobject", CRT("__RTtypeid", objc + 8), tdc);

    // Galaxy offline, GL with no context current (a no-op, still popping its stdcall args).
    CHECK("Galaxy User() = NULL", (call(&c, "Galaxy.dll", "?User@api@galaxy@@YAPAVIUser@12@XZ", 0, NULL), c.eax), 0);
    // FMOD bridge over the silent stub backend: handles, user data, version, empty banks, per-path descriptions.
    setenv("MINA4MAC_AUDIO", "stub", 1);
#define FS(name, ...) (call(&c, "fmodstudio.dll", name, sizeof((uint32_t[]){__VA_ARGS__}) / 4, (uint32_t[]){__VA_ARGS__}), c.eax)
    uint32_t fo = heap_calloc(4, 4), fsys, fev, fev2, finst, fbuf = heap_alloc(64);
    CHECK("FMOD create", FS("?create@System@Studio@FMOD@@SG?AW4FMOD_RESULT@@PAPAV123@I@Z", fo, 0x20105), 0);
    fsys = rd32(fo);
    CHECK("FMOD System isValid", FS("?isValid@System@Studio@FMOD@@QBG_NXZ", fsys) & 0xff, 1);
    CHECK("FMOD isValid(garbage)", FS("?isValid@System@Studio@FMOD@@QBG_NXZ", fbuf) & 0xff, 0);
    FS("?getCoreSystem@System@Studio@FMOD@@QBG?AW4FMOD_RESULT@@PAPAV13@@Z", fsys, fo);
    CHECK("FMOD getVersion pops 8", call(&c, "fmod.dll", "?getVersion@System@FMOD@@QAG?AW4FMOD_RESULT@@PAI@Z", 2,
                                         (uint32_t[]){rd32(fo), fo + 4}), c.esp);
    CHECK("FMOD getVersion", rd32(fo + 4), 0x20105);
    FS("?loadBankFile@System@Studio@FMOD@@QAG?AW4FMOD_RESULT@@PBDIPAPAVBank@23@@Z", fsys, gs("data/audio/x.bank"), 0, fo);
    wr32(fo + 4, 7);
    FS("?getEventCount@Bank@Studio@FMOD@@QBG?AW4FMOD_RESULT@@PAH@Z", rd32(fo), fo + 4);
    CHECK("FMOD bank has no events", rd32(fo + 4), 0);
    FS("?getEvent@System@Studio@FMOD@@QBG?AW4FMOD_RESULT@@PBDPAPAVEventDescription@23@@Z", fsys, gs("event:/a/b"), fo);
    fev = rd32(fo);
    FS("?getEvent@System@Studio@FMOD@@QBG?AW4FMOD_RESULT@@PBDPAPAVEventDescription@23@@Z", fsys, gs("event:/a/b"), fo);
    fev2 = rd32(fo);
    CHECK("FMOD getEvent same path, same handle", fev && fev == fev2, 1);
    FS("?getPath@EventDescription@Studio@FMOD@@QBG?AW4FMOD_RESULT@@PADHPAH@Z", fev, fbuf, 6, fo + 4);
    CHECK("FMOD getPath truncated", !strcmp((char *)P(fbuf), "event") && rd32(fo + 4) == 11, 1);
    CHECK("FMOD getUserProperty not found", FS("?getUserProperty@EventDescription@Studio@FMOD@@QBG?AW4FMOD_RESULT@@PBDPAUFMOD_STUDIO_USER_PROPERTY@@@Z",
                                                fev, gs("x"), fbuf), 74);
    FS("?createInstance@EventDescription@Studio@FMOD@@QBG?AW4FMOD_RESULT@@PAPAVEventInstance@23@@Z", fev, fo);
    finst = rd32(fo);
    FS("?setUserData@EventInstance@Studio@FMOD@@QAG?AW4FMOD_RESULT@@PAX@Z", finst, 0xabcd);
    FS("?getUserData@EventInstance@Studio@FMOD@@QBG?AW4FMOD_RESULT@@PAPAX@Z", finst, fo + 8);
    CHECK("FMOD instance user data", rd32(fo + 8), 0xabcd);
    CHECK("FMOD instance isValid", FS("?isValid@EventInstance@Studio@FMOD@@QBG_NXZ", finst) & 0xff, 1);
    FS("?release@EventInstance@Studio@FMOD@@QAG?AW4FMOD_RESULT@@XZ", finst);
    CHECK("FMOD released instance invalid", FS("?isValid@EventInstance@Studio@FMOD@@QBG_NXZ", finst) & 0xff, 0);
    CHECK("FMOD released instance: invalid handle", FS("?start@EventInstance@Studio@FMOD@@QAG?AW4FMOD_RESULT@@XZ", finst), 30);
    FS("?createInstance@EventDescription@Studio@FMOD@@QBG?AW4FMOD_RESULT@@PAPAVEventInstance@23@@Z", fev, fo);
    CHECK("FMOD reused slot, new handle", rd32(fo) != finst && FS("?isValid@EventInstance@Studio@FMOD@@QBG_NXZ", finst) == 0, 1);
    CHECK("FMOD setParameterByName pops 16", call(&c, "fmodstudio.dll", "?setParameterByName@EventInstance@Studio@FMOD@@QAG?AW4FMOD_RESULT@@PBDM_N@Z",
                                                  4, (uint32_t[]){finst, gs("p"), 0, 0}), c.esp);
    FS("?release@System@Studio@FMOD@@QAG?AW4FMOD_RESULT@@XZ", fsys);
    CHECK("FMOD released system invalid", FS("?isValid@System@Studio@FMOD@@QBG_NXZ", fsys) & 0xff, 0);
    CHECK("glClear, no context: pops 4", call(&c, "opengl32.dll", "glClear", 1, (uint32_t[]){0x4000}), c.esp);
    CHECK("glGetString, no context", (call(&c, "opengl32.dll", "glGetString", 1, (uint32_t[]){0x1f00}), c.eax), 0);
    CHECK("glCreateShader, no context: 0", (call(&c, "opengl32.dll", "glCreateShader", 1, (uint32_t[]){0x8b31}), c.eax), 0);
    CHECK("glShaderSource, no context: pops 16", call(&c, "opengl32.dll", "glShaderSource", 4, (uint32_t[]){1, 1, 0, 0}), c.esp);
    // WININET offline: no session, ERROR_INTERNET_NAME_NOT_RESOLVED.
    CHECK("InternetOpenA: pops 20", call(&c, "WININET.dll", "InternetOpenA", 5, (uint32_t[]){0, 0, 0, 0, 0}), c.esp);
    CHECK("... fails", c.eax, 0);
    CHECK("... last error 12007", K32("GetLastError"), 12007);

    snprintf(p, sizeof p, "rm -rf '%s'", tmp);
    CHECK("cleanup", system(p), 0);
}

static void msvcp120_binding(const char *dll) {
    enum { MSVCP_BASE = 0x18000000 };
    uint32_t entry = rt_map_pe(dll, MSVCP_BASE);
    CHECK("msvcp120 entry", entry, 0x1803b707);
    rt_register_module("MSVCP120.dll", MSVCP_BASE);
    uint32_t xlen = rt_export(MSVCP_BASE, "?_Xlength_error@std@@YAXPBD@Z");
    CHECK("export by name", xlen > MSVCP_BASE && xlen < MSVCP_BASE + 0x71000, 1);
    uint32_t exp = MSVCP_BASE + rd32(MSVCP_BASE + rd32(MSVCP_BASE + 0x3c) + 24 + 96);
    uint32_t ord = rd32(exp + 16), k = 0;  // find the export's ordinal
    char ordname[16];
    for (uint32_t i = 0; i < rd32(exp + 20); i++)
        if (MSVCP_BASE + rd32(MSVCP_BASE + rd32(exp + 28) + 4 * i) == xlen) k = i;
    snprintf(ordname, sizeof ordname, "#%u", ord + k);
    CHECK("export by ordinal", rt_export(MSVCP_BASE, ordname), xlen);
    CHECK("missing export", rt_export(MSVCP_BASE, "nope"), 0);
    rt_bind_imports(EXE_BASE);
    uint32_t nt = EXE_BASE + rd32(EXE_BASE + 0x3c), dir = rd32(nt + 24 + 96 + 8), bound = 0, right = 0;
    for (uint32_t d = EXE_BASE + dir; rd32(d + 12); d += 20) {
        if (strcasecmp((char *)P(EXE_BASE + rd32(d + 12)), "MSVCP120.dll")) continue;
        for (uint32_t i = 0, e; (e = rd32(EXE_BASE + rd32(d) + 4 * i)); i++, bound++)
            right += rd32(EXE_BASE + rd32(d + 16) + 4 * i) == rt_export(MSVCP_BASE, (char *)P(EXE_BASE + e + 2));
    }
    CHECK("MSVCP120 slots -> exports", bound == 141 && right == 141, 1);
}

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: env_test <noita image.bin> <msvcp120.dll>\n"); return 2; }
    rt_init();

    // Heap, directly.
    uint32_t a = heap_alloc(0), b = heap_alloc(0);
    CHECK("malloc(0) unique non-null", a && b && a != b, 1);
    CHECK("small: aligned, in range", (a & 15) == 0 && in_heap(a, 16), 1);
    heap_free(a);
    CHECK("small: freed block reused", heap_alloc(0), a);
    uint32_t big = heap_alloc(1 << 20);
    CHECK("large: page + header aligned", big & 0xfff, 16);
    memset(P(big), 0xab, 1 << 20);
    uint32_t big2 = heap_realloc(big, 3 << 20);
    CHECK("realloc: contents kept", all(big2, 0xab, 1 << 20), 1);
    CHECK("realloc: size", heap_size(big2), 3 << 20);
    CHECK("realloc: shrink in place", heap_realloc(big2, 2 << 20), big2);
    uint32_t l1 = heap_alloc(100000), l2 = heap_alloc(100000), l3 = heap_alloc(100000);
    heap_free(l2); heap_free(l1); heap_free(l3);
    CHECK("large: spans coalesce", heap_alloc(300000), l1);
    uint32_t z = heap_calloc(1000, 7);
    CHECK("calloc: zeroed", all(z, 0, 7000), 1);
    CHECK("calloc: overflow fails", heap_calloc(0x10000, 0x10001), 0);
    CHECK("alloc: too big fails", heap_alloc(0xf0000000u), 0);
    CHECK("resize: in place fits", heap_resize(z, 7008), z);
    CHECK("resize: too big fails", heap_resize(z, 70000), 0);
    int aligned_ok = 1;
    for (uint32_t al = 1; al <= 65536; al *= 2) {
        uint32_t p = heap_aligned_alloc(1000, al);
        aligned_ok &= p && p % al == 0 && in_heap(p, 1000);
        memset(P(p), 1, 1000);
        heap_aligned_free(p);
    }
    CHECK("aligned alloc 1..64K", aligned_ok, 1);
    CHECK("aligned alloc: bad align", heap_aligned_alloc(10, 3), 0);
    CHECK("owns: live / freed", heap_owns(z) * 2 + heap_owns(l2), 2);

    // Heap, from many threads at once.
    pthread_t th[THREADS];
    for (long i = 0; i < THREADS; i++) pthread_create(&th[i], NULL, hammer, (void *)i);
    long bad = 0;
    for (int i = 0; i < THREADS; i++) { void *r; pthread_join(th[i], &r); bad += (long)r; }
    CHECK("threads: no corruption/overlap", bad, 0);

    // Process environment.
    rt_load_image(argv[1], EXE_BASE);
    rt_process_init(EXE_BASE);
    CPU c = {.fpu_cw = 0x27f};
    uint32_t teb = rt_thread_init(&c);
    CHECK("fs_base = TEB", c.fs_base, teb);
    CHECK("fs:[0] SEH chain end", rd32(teb + TEB_EXCEPTION_LIST), 0xffffffff);
    CHECK("fs:[4] stack base = esp", rd32(teb + TEB_STACK_BASE), c.esp);
    CHECK("stack size", rd32(teb + TEB_STACK_BASE) - rd32(teb + TEB_STACK_LIMIT), STACK_SIZE);
    CHECK("fs:[0x18] self", rd32(teb + TEB_SELF), teb);
    CHECK("fs:[0x24] thread id", rd32(teb + TEB_TID), GUEST_PID + 4);
    CHECK("fs:[0x30] PEB", rd32(teb + TEB_PEB), PEB_ADDR);
    CHECK("PEB image base", rd32(PEB_ADDR + PEB_IMAGE_BASE), EXE_BASE);
    CHECK("PEB process heap", rd32(PEB_ADDR + PEB_PROCESS_HEAP), PROCESS_HEAP);
    uint32_t nt = EXE_BASE + rd32(EXE_BASE + 0x3c), tlsdir = EXE_BASE + rd32(nt + 24 + 96 + 72);
    uint32_t tls_start = rd32(tlsdir), tls_len = rd32(tlsdir + 4) - tls_start;
    uint32_t block = rd32(rd32(teb + TEB_TLS_POINTER));
    CHECK("TLS index written", rd32(rd32(tlsdir + 8)), 0);
    CHECK("TLS block from heap", heap_owns(block), 1);
    CHECK("TLS block = template", memcmp(P(block), P(tls_start), tls_len), 0);
    CPU c2 = {0};
    uint32_t teb2 = rt_thread_init(&c2);
    CHECK("2nd thread: own TEB/stack/TLS", teb2 == teb + TEB_SLOT && c2.esp == c.esp + STACK_SLOT
          && rd32(rd32(teb2 + TEB_TLS_POINTER)) != block, 1);
    fflush(stdout);
    pid_t pid = fork();
    if (!pid) { wr32(rd32(teb + TEB_STACK_LIMIT) - 4, 1); _exit(0); }  // overflow into the guard
    int st;
    waitpid(pid, &st, 0);
    CHECK("stack guard faults", WIFSIGNALED(st) && (WTERMSIG(st) == SIGSEGV || WTERMSIG(st) == SIGBUS), 1);

    // The heap through the imports, called the way guest code calls them.
    uint32_t m = CRT("malloc", 40);
    CHECK("malloc", heap_owns(m) && heap_size(m) == 40, 1);
    memset(P(m), 7, 40);
    uint32_t m2 = CRT("realloc", m, 5000);
    CHECK("realloc keeps contents", all(m2, 7, 40), 1);
    CHECK("_msize", CRT("_msize", m2), 5000);
    CHECK("realloc(p, 0) frees", CRT("realloc", m2, 0) == 0 && !heap_owns(m2), 1);
    uint32_t cz = CRT("calloc", 10, 10);
    CHECK("calloc", all(cz, 0, 100), 1);
    CRT("free", cz);
    CHECK("free", heap_owns(cz), 0);
    uint32_t am = CRT("_aligned_malloc", 100, 256);
    CHECK("_aligned_malloc", am && am % 256 == 0, 1);
    CRT("_aligned_free", am);
    uint32_t n = CRT("??2@YAPAXI@Z", 24);
    CHECK("operator new", heap_owns(n), 1);
    CRT("??3@YAXPAX@Z", n);
    CHECK("operator delete", heap_owns(n), 0);
    CHECK("GetProcessHeap: stdcall pops 0", call(&c, "KERNEL32.dll", "GetProcessHeap", 0, NULL), c.esp);
    CHECK("GetProcessHeap", c.eax, PROCESS_HEAP);
    uint32_t h = K32("HeapAlloc", PROCESS_HEAP, 8, 64);
    CHECK("HeapAlloc ZERO_MEMORY", heap_owns(h) && all(h, 0, 64), 1);
    CHECK("HeapAlloc: stdcall pops 12", call(&c, "KERNEL32.dll", "HeapAlloc", 3, (uint32_t[]){PROCESS_HEAP, 0, 8}), c.esp);
    memset(P(h), 9, 64);
    uint32_t h2 = K32("HeapReAlloc", PROCESS_HEAP, 8, h, 128);
    CHECK("HeapReAlloc ZERO_MEMORY", all(h2, 9, 64) && all(h2 + 64, 0, 64), 1);
    CHECK("HeapReAlloc IN_PLACE fails", K32("HeapReAlloc", PROCESS_HEAP, 0x10, h2, 100000), 0);
    CHECK("HeapSize", K32("HeapSize", PROCESS_HEAP, 0, h2), 128);
    CHECK("HeapFree", K32("HeapFree", PROCESS_HEAP, 0, h2) == 1 && !heap_owns(h2), 1);

    // CRT startup.
    crt_init(3, (char *[]){"mina4mac", "-x", "a b", NULL});
    uint32_t pargc = heap_alloc(12), pargv = pargc + 4, penv = pargc + 8;
    CHECK("__getmainargs", CRT("__getmainargs", pargc, pargv, penv, 0, 0), 0);
    uint32_t av = rd32(pargv);
    CHECK("argc/argv", rd32(pargc) == 3 && !strcmp((char *)P(rd32(av)), "noita.exe")
          && !strcmp((char *)P(rd32(av + 4)), "-x") && !strcmp((char *)P(rd32(av + 8)), "a b") && !rd32(av + 12), 1);
    CHECK("envp empty", rd32(rd32(penv)), 0);
    CHECK("_acmdln", strcmp((char *)P(rd32(rt_thunk("MSVCR120.dll", "_acmdln"))), "\"noita.exe\" -x \"a b\""), 0);
    uint32_t cmdw = K32("GetCommandLineW");
    char cmd8[64];
    CHECK("GetCommandLineW", utf16_to_utf8(cmdw, cmd8, sizeof cmd8) && !strcmp(cmd8, "\"noita.exe\" -x \"a b\""), 1);
    CHECK("GetCommandLineA", K32("GetCommandLineA"), rd32(rt_thunk("MSVCR120.dll", "_acmdln")));

    // SDL2main's WinMain helpers: turn the UTF-16 command line into UTF-8 for SDL_main's argv.
    uint32_t n16 = SDL("SDL_wcslen", cmdw);
    CHECK("SDL_wcslen", n16, strlen(cmd8));
    uint32_t to = guest_strdup("UTF-8"), from = guest_strdup("UTF-16LE");
    uint32_t u8 = SDL("SDL_iconv_string", to, from, cmdw, 2 * (n16 + 1));
    CHECK("SDL_iconv_string UTF-16LE", u8 && !strcmp((char *)P(u8), cmd8), 1);
    CHECK("SDL_iconv_string unsupported", SDL("SDL_iconv_string", from, to, u8, 4), 0);
    CHECK("SDL_isspace", SDL("SDL_isspace", '\v') * 2 + SDL("SDL_isspace", 'x'), 2);
    uint32_t sm = SDL("SDL_malloc", 0);
    CHECK("SDL_malloc(0)", sm && heap_owns(sm), 1);
    SDL("SDL_free", sm);
    CHECK("SDL_free", heap_owns(sm), 0);
    uint32_t tab = heap_calloc(4, 4);
    wr32(tab, G_A); wr32(tab + 8, G_B);
    CRT("_initterm", tab, tab + 16);
    CHECK("_initterm: skips nulls", strcmp(CALLS, "ab"), 0);
    memset(CALLS, 0, sizeof CALLS);
    wr32(tab + 4, G_SEVEN);
    CHECK("_initterm_e: result", CRT("_initterm_e", tab, tab + 16), 7);
    CHECK("_initterm_e: stops at non-zero", strcmp(CALLS, "a7"), 0);
    memset(CALLS, 0, sizeof CALLS);
    CRT("_onexit", G_A); CRT("_onexit", G_B);
    CRT("_cexit");
    CHECK("_onexit/_cexit: reverse order", strcmp(CALLS, "ba"), 0);
    uint32_t cw = heap_alloc(4);
    CRT("_controlfp_s", cw, 0x300, 0x300);  // _RC_CHOP
    CHECK("_controlfp_s: chop", c.fpu_cw, 0xe7f);
    CHECK("_controlfp_s: current", rd32(cw), 0x9031f);  // all masked, _PC_53, _RC_CHOP
    CRT("_controlfp_s", 0, 0x20100, 0x30300);  // _PC_24, _RC_DOWN
    CHECK("_controlfp_s: pc24 down", c.fpu_cw, 0x47f);
    CRT("_controlfp_s", cw, 0, 0);
    CHECK("_controlfp_s: read only", rd32(cw) == 0xa011f && c.fpu_cw == 0x47f, 1);
    CRT("_controlfp_s", 0, 0x10000, 0x30300);
    CHECK("_controlfp_s: back to default", c.fpu_cw, 0x27f);

    // KERNEL32 identity and time.
    CHECK("GetCurrentThreadId", K32("GetCurrentThreadId"), GUEST_PID + 4);
    CHECK("GetCurrentProcessId", K32("GetCurrentProcessId"), GUEST_PID);
    K32("SetLastError", 1234);
    CHECK("Set/GetLastError", K32("GetLastError"), 1234);
    CHECK("SetLastError: stdcall pops 4", call(&c, "KERNEL32.dll", "SetLastError", 1, (uint32_t[]){0}), c.esp);
    CHECK("Encode/DecodePointer", K32("DecodePointer", K32("EncodePointer", 0x12345678)), 0x12345678);
    CHECK("IsProcessorFeaturePresent SSE2/3DNow", K32("IsProcessorFeaturePresent", 10) * 2
          + K32("IsProcessorFeaturePresent", 7), 2);
    uint32_t t = heap_alloc(16);
    K32("GetSystemTimeAsFileTime", t);
    CHECK("FILETIME after 2020", rd64(t) > 132223104000000000ull, 1);  // 2020-01-01
    K32("QueryPerformanceFrequency", t);
    uint64_t freq = rd64(t);
    K32("QueryPerformanceCounter", t); usleep(2000); K32("QueryPerformanceCounter", t + 8);
    uint64_t dt = (rd64(t + 8) - rd64(t)) * 1000 / freq;  // ms
    CHECK("QueryPerformanceCounter ~2 ms", dt >= 2 && dt < 100, 1);
    wr64(t, 132223104001230000ull + 36000000000ull * 13);  // 2020-01-01 13:00:00.123 UTC, a Wednesday
    uint32_t sys = heap_alloc(16), lt = heap_alloc(16);
    CHECK("FileTimeToSystemTime", K32("FileTimeToSystemTime", t, sys), 1);
    CHECK("... 2020-01-01 (Wed) 13:00:00.123", rd16(sys) == 2020 && rd16(sys + 2) == 1 && rd16(sys + 4) == 3
          && rd16(sys + 6) == 1 && rd16(sys + 8) == 13 && rd16(sys + 10) == 0 && rd16(sys + 12) == 0 && rd16(sys + 14) == 123, 1);
    time_t secs = 1577883600;  // the same instant
    struct tm tm;
    localtime_r(&secs, &tm);
    CHECK("SystemTimeToTzSpecificLocalTime (host zone)", K32("SystemTimeToTzSpecificLocalTime", 0, sys, lt), 1);
    CHECK("... hour and ms", rd16(lt + 8) == tm.tm_hour && rd16(lt + 6) == tm.tm_mday && rd16(lt + 14) == 123, 1);
    K32("GetLocalTime", lt);
    CHECK("GetLocalTime year", rd16(lt) >= 2020 && rd16(lt + 2) >= 1 && rd16(lt + 2) <= 12, 1);
    CHECK("timeBeginPeriod", (call(&c, "WINMM.dll", "timeBeginPeriod", 1, (uint32_t[]){1}), c.eax), 0);
    CHECK("FindFirstChangeNotificationW fails", K32("FindFirstChangeNotificationW", gws("."), 0, 1), 0xffffffff);
    uint32_t wbuf = heap_calloc(2, 16), ab2 = gs("a\xc3\xa9");  // "aé"
    CHECK("MultiByteToWideChar size (with NUL)", K32("MultiByteToWideChar", 0, 0, ab2, -1, 0, 0), 3);
    CHECK("MultiByteToWideChar", K32("MultiByteToWideChar", 0, 0, ab2, 4, wbuf, 16) == 3 && gwstreq(wbuf, "a\xc3\xa9"), 1);
    CHECK("MultiByteToWideChar no NUL", K32("MultiByteToWideChar", 0, 0, ab2, 1, wbuf, 16) == 1 && rd16(wbuf + 2) == 0xe9, 1);
    CHECK("MultiByteToWideChar small buffer", K32("MultiByteToWideChar", 0, 0, ab2, -1, wbuf, 2) == 0
          && K32("GetLastError") == 122, 1);
    CHECK("CreateProcessW fails", K32("CreateProcessW", gws("x.exe"), 0, 0, 0, 0, 0, 0, 0, 0, 0) == 0
          && K32("GetLastError") == 2, 1);
    // Offline sockets: the game's Socket::open (0x4326d0) gives up when WSAStartup fails.
#define WS(name, ...) (call(&c, "WS2_32.dll", name, sizeof((uint32_t[]){__VA_ARGS__}) / 4, (uint32_t[]){__VA_ARGS__}), c.eax)
    CHECK("WSAStartup fails (WSASYSNOTREADY)", WS("WSAStartup", 0x202, heap_calloc(1, 400)), 10091);
    CHECK("socket: INVALID_SOCKET", WS("socket", 2, 1, 0) == 0xffffffff && (call(&c, "WS2_32.dll", "WSAGetLastError", 0, NULL), c.eax) == 10093, 1);
    CHECK("gethostbyname: NULL", WS("gethostbyname", gs("irc.chat.twitch.tv")), 0);
    CHECK("connect/send/recv fail", WS("connect", 1, 0, 16) & WS("send", 1, 0, 0, 0) & WS("recv", 1, 0, 0, 0), 0xffffffff);
    CHECK("htons", WS("htons", 6667), 0x0b1a);
    CHECK("WSACleanup: stdcall pops 0", call(&c, "WS2_32.dll", "WSACleanup", 0, NULL), c.esp);
#undef WS
    CHECK("GetActiveWindow: none", (call(&c, "USER32.dll", "GetActiveWindow", 0, NULL), c.eax), 0);
    CHECK("GetOpenFileNameA: cancelled", (call(&c, "COMDLG32.dll", "GetOpenFileNameA", 1, (uint32_t[]){heap_calloc(1, 88)}), c.eax), 0);
    CHECK("ShellExecuteA missing file", (call(&c, "SHELL32.dll", "ShellExecuteA", 6,
                                               (uint32_t[]){0, 0, gs("no\\such\\file"), 0, 0, 5}), c.eax), 2);
    CHECK("ShellExecuteA unknown verb", (call(&c, "SHELL32.dll", "ShellExecuteA", 6,
                                               (uint32_t[]){0, gs("print"), gs("https://noitagame.com"), 0, 0, 5}), c.eax), 31);
    initializer_imports(&c, &c2);
    sdl_main_imports(&c);
    msvcp120_binding(argv[2]);
    return fails != 0;
}
