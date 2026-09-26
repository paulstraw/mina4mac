// LuaJIT bridge test (built and run by tools/check.sh as luatest). Drives lua51.c through the import thunks
// the way guest code does, with "recompiled" guest C functions below: state creation and every luaopen_*
// library (luaL_openlibs, and the game's pushcclosure(luaopen_* thunk) + lua_call), guest closures with
// upvalues, errors raised in guest code and caught by lua_pcall nested three deep (and by Lua's pcall), guest
// register/SEH state after unwinding, userdata round trips, luaL_ref, lua_tonumber in st0, strings and
// lua_Debug, a JIT-hot loop, a second guest thread, and the mod sandbox.
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "heap.h"
#include "host.h"
#include "proc.h"

enum { GLOBALS = -10002, REGISTRY = -10000 };
#define UPV(k) ((uint32_t)(GLOBALS - (k)))  // the guest's lua_upvalueindex(k)

// Call lua51!name through its thunk (looked up once per call site, as guest code reads its IAT slot).
#define LUA(c, name, ...)                                                                             \
    ({                                                                                                \
        static uint32_t thunk_;                                                                       \
        if (!thunk_) thunk_ = rt_thunk("lua51.dll", name);                                            \
        uint32_t args_[] = {__VA_ARGS__};                                                             \
        call_guest(c, thunk_, sizeof args_ / 4, args_);                                               \
    })

static uint32_t STRS[64];
static int NSTRS;
static uint32_t gstr(const char *s) {  // a guest copy of s (kept for the whole test)
    uint32_t g = heap_alloc(strlen(s) + 1);
    strcpy((char *)P(g), s);
    if (NSTRS < 64) STRS[NSTRS++] = g;
    return g;
}
static uint32_t dlo(double d) { uint64_t v; memcpy(&v, &d, 8); return (uint32_t)v; }
static uint32_t dhi(double d) { uint64_t v; memcpy(&v, &d, 8); return v >> 32; }

// "Recompiled" guest lua_CFunctions: int __cdecl f(lua_State *L). Each pops its return address like `ret`.
enum { G_COUNTER = 0x300000, G_NEST = 0x300010, G_WHERE = 0x300020, G_TID = 0x300030, G_ADD = 0x300040 };

// counter(): upvalue 1 += upvalue 2; returns the new value.
static void F_counter(CPU *c) {
    uint32_t L = rd32(c->esp + 4);
    int32_t n = (int32_t)LUA(c, "lua_tointeger", L, UPV(1)) + (int32_t)LUA(c, "lua_tointeger", L, UPV(2));
    LUA(c, "lua_pushinteger", L, (uint32_t)n);
    LUA(c, "lua_replace", L, UPV(1));
    LUA(c, "lua_pushinteger", L, (uint32_t)n);
    c->eax = 1;
    c->esp += 4;
}

// nest(n): like recompiled C++ code, clobbers ebx and registers an SEH frame on its stack first. n == 0
// raises "boom"; otherwise it lua_pcalls nest(n - 1), checks its own state survived, appends "<n" to the
// error and re-raises it, except at n == 3, which returns it.
enum { SEH_OLD = 0xfeedf00d };
static int NEST_OK[4];
static void F_nest(CPU *c) {
    uint32_t L = rd32(c->esp + 4), frame = c->esp - 8;
    uint32_t n = LUA(c, "lua_tointeger", L, 1), saved_ebx = c->ebx, saved_seh = rd32(c->fs_base);
    c->esp -= 16;  // locals, holding the SEH record
    wr32(frame, saved_seh);
    wr32(c->fs_base, frame);
    c->ebx = 0xb0000000 | n;
    if (n == 0) {
        LUA(c, "lua_pushstring", L, gstr("boom"));
        LUA(c, "lua_error", L);
    }
    LUA(c, "lua_getfield", L, GLOBALS, gstr("nest"));
    LUA(c, "lua_pushinteger", L, n - 1);
    uint32_t esp = c->esp, st = LUA(c, "lua_pcall", L, 1, 1, 0);
    NEST_OK[n] = st == 2 /* LUA_ERRRUN */ && c->ebx == (0xb0000000 | n) && rd32(c->fs_base) == frame && c->esp == esp;
    uint32_t msg = LUA(c, "lua_tolstring", L, -1, 0);
    LUA(c, "lua_pushfstring", L, gstr("%s<%d"), msg, n);
    if (n < 3) LUA(c, "lua_error", L);
    wr32(c->fs_base, saved_seh);
    c->ebx = saved_ebx;
    c->esp += 16;
    c->eax = 1;
    c->esp += 4;
}

// where(): the caller's currentline and short_src from lua_getstack(1) + lua_getinfo("Sl"), via a guest
// lua_Debug; returns "<what> <short_src>:<line>".
static void F_where(CPU *c) {
    uint32_t L = rd32(c->esp + 4), ar = heap_calloc(1, 100);
    uint32_t ok = LUA(c, "lua_getstack", L, 1, ar) && LUA(c, "lua_getinfo", L, gstr("Sl"), ar);
    char buf[160];
    snprintf(buf, sizeof buf, "%u %s %s:%d", ok, (const char *)P(rd32(ar + 12)), (const char *)P(ar + 36), (int)rd32(ar + 20));
    LUA(c, "lua_pushstring", L, gstr(buf));
    heap_free(ar);
    c->eax = 1;
    c->esp += 4;
}

// tid(): the calling guest thread's id, from its TEB.
static void F_tid(CPU *c) {
    uint32_t L = rd32(c->esp + 4);
    LUA(c, "lua_pushinteger", L, rd32(c->fs_base + TEB_TID));
    c->eax = 1;
    c->esp += 4;
}

// add(a, b): a + b, through lua_tonumber (st0) and lua_pushnumber (a double argument).
static void F_add(CPU *c) {
    uint32_t L = rd32(c->esp + 4);
    LUA(c, "lua_tonumber", L, 1);
    LUA(c, "lua_tonumber", L, 2);
    double s = ST(0) + ST(1);
    st_pop(c), st_pop(c);
    LUA(c, "lua_pushnumber", L, dlo(s), dhi(s));
    c->eax = 1;
    c->esp += 4;
}

const FnEntry FN_TABLE[] = {{G_COUNTER, F_counter}, {G_NEST, F_nest}, {G_WHERE, F_where}, {G_TID, F_tid}, {G_ADD, F_add}};
const int FN_COUNT = 5;

static int fails;
#define CHECK(name, got, want)                                                                        \
    do {                                                                                              \
        uint64_t g_ = (got), w_ = (want);                                                             \
        printf("%-36s %s got %#llx want %#llx\n", name, g_ == w_ ? "ok  " : "FAIL", g_, w_);         \
        fails += g_ != w_;                                                                            \
    } while (0)
#define CHECK_STR(name, got, want)                                                                    \
    do {                                                                                              \
        const char *g_ = (got), *w_ = (want);                                                         \
        int ok_ = g_ && !strcmp(g_, w_);                                                              \
        printf("%-36s %s got \"%s\" want \"%s\"\n", name, ok_ ? "ok  " : "FAIL", g_ ? g_ : "(null)", w_); \
        fails += !ok_;                                                                                \
    } while (0)

// Run a chunk under lua_pcall; returns its first result as a string (a static copy) or the error.
static const char *run(CPU *c, uint32_t L, const char *code) {
    static char out[256];
    uint32_t st = LUA(c, "luaL_loadstring", L, gstr(code));
    if (!st) st = LUA(c, "lua_pcall", L, 0, 1, 0);
    uint32_t s = LUA(c, "lua_tolstring", L, -1, 0);
    snprintf(out, sizeof out, "%s%s", st ? "error: " : "", s ? (const char *)P(s) : "(not a string)");
    LUA(c, "lua_settop", L, -2);
    return out;
}

static void setglobal_fn(CPU *c, uint32_t L, const char *name, uint32_t fn) {
    LUA(c, "lua_pushcclosure", L, fn, 0);
    LUA(c, "lua_setfield", L, GLOBALS, gstr(name));
}

static void *second_thread(void *p) {
    uint32_t *out = p;
    CPU c = {.fpu_cw = 0x27f};
    rt_thread_init(&c);
    uint32_t L = LUA(&c, "luaL_newstate");
    setglobal_fn(&c, L, "tid", G_TID);
    out[0] = (uint32_t)atoi(run(&c, L, "return tid()"));
    out[1] = rd32(c.fs_base + TEB_TID);
    LUA(&c, "lua_close", L);
    return NULL;
}

static const uint8_t PATCH[10] = {0xc7, 0x05, 0, 0, 0, 0, 0, 0, 0, 0};  // mov dword ptr [0], 0

// In a child process: patch `thunk` like the game's sandbox, run `code` in a state with all libraries, and
// return the child's exit code (the bridge exits with 10 on a sandboxed call).
static int sandboxed(CPU *c, const char *thunk, const char *code, int patch_before_openlibs) {
    fflush(stdout);
    pid_t pid = fork();
    if (!pid) {
        uint32_t L = LUA(c, "luaL_newstate");
        if (patch_before_openlibs) memcpy(P(rt_thunk("lua51.dll", thunk)), PATCH, 10);
        LUA(c, "luaL_openlibs", L);
        memcpy(P(rt_thunk("lua51.dll", thunk)), PATCH, 10);
        const char *r = run(c, L, code);
        _exit(strcmp(r, "ok") ? 1 : 0);
    }
    int status;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

int main(void) {
    rt_init();
    CPU c = {.fpu_cw = 0x27f};
    rt_thread_init(&c);

    // State creation and all libraries.
    uint32_t L = LUA(&c, "luaL_newstate");
    CHECK("newstate: L in the guest heap", L >= HEAP_LO && L < HEAP_HI, 1);
    LUA(&c, "luaL_openlibs", L);
    CHECK_STR("openlibs: all libraries",
              run(&c, L, "return type(print)..type(table.insert)..type(string.format)..type(math.sin)..type(io.open)"
                         "..type(os.time)..type(require)..type(debug.traceback)..type(bit.band)..type(jit.status)"
                         "..type(require('ffi').new)..type(package.loaded.string)"),
              "functionfunctionfunctionfunctionfunctionfunctionfunctionfunctionfunctionfunctionfunctiontable");
    CHECK_STR("openlibs: io and os work", run(&c, L, "io.write('') return type(os.time())"), "number");
    CHECK_STR("openlibs: jit on", run(&c, L, "return tostring(jit.status()) .. ' ' .. jit.arch"), "true arm64");

    // The game's way: push each luaopen_* thunk as a C function and lua_call it with the library name.
    uint32_t L2 = LUA(&c, "luaL_newstate");
    static const char *LIBS[][2] = {{"", "luaopen_base"}, {"table", "luaopen_table"}, {"string", "luaopen_string"},
                                    {"math", "luaopen_math"}, {"bit", "luaopen_bit"}, {"jit", "luaopen_jit"}};
    for (int i = 0; i < 6; i++) {
        LUA(&c, "lua_pushcclosure", L2, rt_thunk("lua51.dll", LIBS[i][1]), 0);
        LUA(&c, "lua_pushstring", L2, gstr(LIBS[i][0]));
        LUA(&c, "lua_call", L2, 1, 0);
    }
    CHECK_STR("luaopen_* thunks via lua_call",
              run(&c, L2, "return type(print)..type(table.insert)..type(string.format)..type(math.sin)..type(bit.bxor)"
                          "..type(jit.on)..type(io)..type(os)"),
              "functionfunctionfunctionfunctionfunctionfunctionnilnil");
    LUA(&c, "lua_close", L2);

    // Guest closures with upvalues.
    LUA(&c, "lua_pushinteger", L, 10);
    LUA(&c, "lua_pushinteger", L, 5);
    LUA(&c, "lua_pushcclosure", L, G_COUNTER, 2);
    LUA(&c, "lua_setfield", L, GLOBALS, gstr("counter"));
    CHECK_STR("closure: upvalues", run(&c, L, "counter(); counter(); return counter()"), "25");
    CHECK("closure: stack balanced", LUA(&c, "lua_gettop", L), 0);

    // Errors in guest code: lua_pcall nested three deep, each level through a guest frame.
    setglobal_fn(&c, L, "nest", G_NEST);
    uint32_t esp = c.esp;
    c.ebx = 0x1234;
    CHECK_STR("errors: nested lua_pcall", run(&c, L, "return nest(3)"), "boom<1<2<3");
    CHECK("errors: level 1 state restored", NEST_OK[1], 1);
    CHECK("errors: level 2 state restored", NEST_OK[2], 1);
    CHECK("errors: level 3 state restored", NEST_OK[3], 1);
    CHECK("errors: outer ebx/esp/SEH", c.ebx == 0x1234 && c.esp == esp && rd32(c.fs_base) == 0xffffffff, 1);
    // Caught by Lua's pcall: the guest frames are abandoned inside a successful lua_pcall.
    CHECK_STR("errors: Lua pcall of guest code", run(&c, L, "local ok, e = pcall(nest, 0) return tostring(ok) .. e"), "falseboom");
    CHECK("errors: ebx/esp/SEH after Lua pcall", c.ebx == 0x1234 && c.esp == esp && rd32(c.fs_base) == 0xffffffff, 1);
    CHECK_STR("errors: Lua error in lua_pcall", run(&c, L, "error('x', 0)"), "error: x");

    // Userdata.
    uint32_t u = LUA(&c, "lua_newuserdata", L, 16);
    CHECK("userdata: in the guest heap", u >= HEAP_LO && u < HEAP_HI, 1);
    wr32(u, 0x12345678);
    CHECK("userdata: touserdata", LUA(&c, "lua_touserdata", L, -1), u);
    CHECK("userdata: topointer", LUA(&c, "lua_topointer", L, -1), u);
    LUA(&c, "lua_setfield", L, GLOBALS, gstr("ud"));
    LUA(&c, "luaL_loadstring", L, gstr("return ud"));
    LUA(&c, "lua_pcall", L, 0, 1, 0);
    CHECK("userdata: through Lua", LUA(&c, "lua_touserdata", L, -1) == u && rd32(u) == 0x12345678, 1);
    LUA(&c, "lua_settop", L, 0);
    LUA(&c, "lua_pushlightuserdata", L, 0x1234);
    CHECK("light userdata: type", LUA(&c, "lua_type", L, -1), 2);
    CHECK("light userdata: touserdata", LUA(&c, "lua_touserdata", L, -1), 0x1234);
    CHECK("light userdata: topointer", LUA(&c, "lua_topointer", L, -1), 0x1234);
    LUA(&c, "lua_settop", L, 0);

    // luaL_ref.
    LUA(&c, "lua_createtable", L, 0, 0);
    uint32_t t = LUA(&c, "lua_topointer", L, -1);
    uint32_t ref = LUA(&c, "luaL_ref", L, REGISTRY);
    CHECK("luaL_ref: stack popped", LUA(&c, "lua_gettop", L), 0);
    LUA(&c, "lua_rawgeti", L, REGISTRY, ref);
    CHECK("luaL_ref: rawgeti gives it back", LUA(&c, "lua_topointer", L, -1) == t && t >= HEAP_LO, 1);
    LUA(&c, "luaL_unref", L, REGISTRY, ref);
    LUA(&c, "lua_settop", L, 0);

    // Numbers: lua_tonumber returns in st0; lua_tointeger is x86 LuaJIT's lj_num2bit (round to nearest even,
    // wrap modulo 2^32), so RegisterSpawnFunction( 0xff6d934c, ... ) gets its color back.
    LUA(&c, "lua_pushnumber", L, dlo(2.5), dhi(2.5));
    int top = c.st_top;
    LUA(&c, "lua_tonumber", L, -1);
    CHECK("tonumber: pushed st0", c.st_top, (top - 1) & 7);
    CHECK("tonumber: value", c.st[c.st_top] == 2.5, 1);
    st_pop(&c);
    static const struct { double v; uint32_t want; } TOINT[] = {
        {3.9, 4}, {-3.9, 0xfffffffc}, {2.5, 2}, {3.5, 4}, {4285371212.0, 0xff6d934c}, {1e10, 0x540be400}};
    for (int i = 0; i < 6; i++) {
        LUA(&c, "lua_pushnumber", L, dlo(TOINT[i].v), dhi(TOINT[i].v));
        CHECK("tointeger", LUA(&c, "lua_tointeger", L, -1), TOINT[i].want);
    }
    LUA(&c, "lua_pushstring", L, gstr("42"));
    CHECK("tointeger: string", LUA(&c, "lua_tointeger", L, -1), 42);
    setglobal_fn(&c, L, "add", G_ADD);
    CHECK_STR("guest add via st0", run(&c, L, "return add(1.25, 2)"), "3.25");
    LUA(&c, "lua_settop", L, 0);

    // Strings and the debug interface.
    LUA(&c, "lua_pushlstring", L, gstr("abcdef"), 3);
    uint32_t len = heap_alloc(4), s = LUA(&c, "lua_tolstring", L, -1, len);
    CHECK_STR("tolstring", (const char *)P(s), "abc");
    CHECK("tolstring: len", rd32(len), 3);
    CHECK("tolstring: in guest memory", s >= HEAP_LO && s < HEAP_HI, 1);
    CHECK_STR("typename", (const char *)P(LUA(&c, "lua_typename", L, 3)), "number");
    s = LUA(&c, "lua_pushfstring", L, gstr("%s:%d %f %c%%"), gstr("src"), 42, dlo(0.5), dhi(0.5), 'x');
    CHECK_STR("pushfstring", (const char *)P(s), "src:42 0.5 x%");
    setglobal_fn(&c, L, "where", G_WHERE);
    uint32_t chunk = gstr("local x = 1\n\nlocal r = where() return r");
    LUA(&c, "luaL_loadbufferx", L, chunk, (uint32_t)strlen((char *)P(chunk)), gstr("=test"), 0);
    LUA(&c, "lua_pcall", L, 0, 1, 0);
    CHECK_STR("getstack/getinfo", (const char *)P(LUA(&c, "lua_tolstring", L, -1, 0)), "1 main test:3");
    LUA(&c, "lua_settop", L, 0);

    // JIT: a hot loop gets compiled (traces exist) and runs fast; also a hot loop calling guest code.
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    CHECK_STR("jit: hot loop", run(&c, L, "local s = 0 for i = 1, 1e8 do s = s + i % 7 end return s"), "299999997");
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ns = ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / 1e8;
    CHECK_STR("jit: traces compiled", run(&c, L, "return tostring(require('jit.util').traceinfo(1) ~= nil)"), "true");
    CHECK("jit: < 5 ns per iteration", ns < 5, 1);
    printf("jit: %.2f ns per loop iteration\n", ns);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    CHECK_STR("jit: hot loop calling guest code", run(&c, L, "local s = 0 for i = 1, 1e6 do s = add(s, 1) end return s"), "1000000");
    clock_gettime(CLOCK_MONOTONIC, &t1);
    printf("guest call: %.1f ns per call\n", ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / 1e6);

    // A second guest thread with its own CPU calls guest code from its own state.
    pthread_t th;
    uint32_t out[2];
    pthread_create(&th, NULL, second_thread, out);
    pthread_join(th, NULL);
    CHECK("thread: guest code on its own CPU", out[0] == out[1] && out[0] != rd32(c.fs_base + TEB_TID), 1);

    // Mod sandbox.
    CHECK("sandbox: unpatched os.time", sandboxed(&c, "luaL_openlibs", "os.time() return 'ok'", 0), 0);
    CHECK("sandbox: os.time while patched", sandboxed(&c, "luaopen_os", "os.time() return 'ok'", 0), 10);
    CHECK("sandbox: file method while patched", sandboxed(&c, "luaopen_io", "io.stdout:write('') return 'ok'", 0), 10);
    CHECK("sandbox: require while patched", sandboxed(&c, "luaopen_package", "require('string') return 'ok'", 0), 10);
    CHECK("sandbox: other libs unaffected", sandboxed(&c, "luaopen_os", "string.format('') io.write('') return 'ok'", 0), 0);
    CHECK("sandbox: luaL_openlibs while patched", sandboxed(&c, "luaL_openlibs", "return 'ok'", 1), 10);

    LUA(&c, "lua_close", L);
    printf("%s\n", fails ? "FAILED" : "all ok");
    return fails != 0;
}
