// lua51.dll (the game's LuaJIT 2.0.4) bridged to a host ARM64 LuaJIT 2.1 with the JIT enabled
// (third_party/luajit, built by tools/build_all.py). The imports are cdecl.
//   - Memory: every state allocates from the guest heap (lua_newstate with guest_alloc), so every LuaJIT
//     object, and every string or pointer handed to the guest, is at a 32-bit guest address. Guest pointers
//     come in as MEM + p; pointers go out as host - MEM (gp aborts if one lies outside guest memory).
//     Static host strings (lua_typename, lua_getinfo's what/namewhat) are handed out as interned guest copies.
//     Light userdata is the raw 32-bit value either way.
//   - C functions: lua_pushcclosure(L, guestfn, n) pushes the host trampoline with guestfn as a hidden first
//     upvalue, so guest upvalue index lua_upvalueindex(k) is host lua_upvalueindex(k + 1) (ix). The trampoline
//     calls guestfn on the thread's guest CPU (rt_thread_cpu). Pushing the *thunk* of an imported lua51
//     function (the game opens its libraries by pushing the luaopen_* thunks) pushes the host function.
//   - Errors are LuaJIT's external unwinding (C++-style, through the recompiled frames' compact unwind
//     info). Guest frames between a trampoline and the catching pcall are abandoned, so the trampoline
//     restores the guest registers, esp, fs:[0] and x87 state on the way out (a cleanup, hence -fexceptions),
//     and lua_pcall does the same when it returns an error. Guest C++ destructors in abandoned frames don't
//     run; LuaJIT on Windows would run them.
//   - Mod sandbox: the game disables LuaJIT's io/os/package/debug/ffi libraries by overwriting their
//     functions with `mov dword ptr [0], 0` (0x7ee720), which lands on the thunks. A library counts as
//     patched while its luaopen_* thunk holds those bytes (the game patches and restores them all together).
//     Opening a patched library, or calling any C function of an opened one (wrapped by sandbox_wrap) while
//     it is patched, aborts like the original's access violation would.
#include <os/lock.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

#include "heap.h"
#include "hle.h"
#include "host.h"
#include "proc.h"

// Host pointer (inside guest memory, or NULL) -> guest address.
static uint32_t gp(const void *p) {
    if (!p) return 0;
    uint64_t d = (uint64_t)((const uint8_t *)p - MEM);
    if (d >> 32) {
        fprintf(stderr, "lua51: host pointer %p handed to the guest is outside guest memory\n", p);
        abort();
    }
    return (uint32_t)d;
}

// A guest copy of a host string that may be static host data (interned, never freed).
static uint32_t guest_const(const char *s) {
    if (!s) return 0;
    if ((uint64_t)((const uint8_t *)s - MEM) >> 32 == 0) return (uint32_t)((const uint8_t *)s - MEM);
    static os_unfair_lock lock = OS_UNFAIR_LOCK_INIT;
    static struct { const char *host; uint32_t guest; } tab[64];
    static int n;
    os_unfair_lock_lock(&lock);
    uint32_t g = 0;
    for (int i = 0; i < n && !g; i++)
        if (!strcmp(tab[i].host, s)) g = tab[i].guest;
    if (!g) {
        g = guest_strdup(s);
        if (n < 64) tab[n++] = (typeof(tab[0])){strdup(s), g};  // beyond 64 distinct strings: copies leak
    }
    os_unfair_lock_unlock(&lock);
    return g;
}

static void *guest_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
    (void)ud, (void)osize;
    if (!nsize) {
        heap_free(gp(ptr));
        return NULL;
    }
    if (nsize >= HEAP_HI - HEAP_LO) return NULL;
    uint32_t p = heap_realloc(gp(ptr), (uint32_t)nsize);
    return p ? P(p) : NULL;
}

static int panic(lua_State *L) {
    const char *msg = lua_tostring(L, -1);
    fprintf(stderr, "lua51: unprotected error in call to Lua API (%s)\n", msg ? msg : "?");
    exit(9);
}

// Guest arguments. Stack indices below LUA_GLOBALSINDEX are guest upvalue indices, shifted past the
// trampoline's hidden upvalue.
#define LS(n) ((lua_State *)P(ARG(n)))
static inline int ix(int i) { return i < LUA_GLOBALSINDEX ? i - 1 : i; }
#define IX(n) ix((int)ARG(n))
#define INT(n) ((int)ARG(n))

// Guest register state that abandoned guest frames (a Lua error unwinding through them) leave behind.
typedef struct {
    CPU *c;
    uint32_t ebx, esi, edi, ebp, esp, seh;
    int st_top;
    uint16_t fpu_cw;
} Saved;
static Saved save(CPU *c) {
    return (Saved){c, c->ebx, c->esi, c->edi, c->ebp, c->esp, rd32(c->fs_base + TEB_EXCEPTION_LIST), c->st_top, c->fpu_cw};
}
static void restore(Saved *s) {
    CPU *c = s->c;
    c->ebx = s->ebx, c->esi = s->esi, c->edi = s->edi, c->ebp = s->ebp, c->esp = s->esp;
    wr32(c->fs_base + TEB_EXCEPTION_LIST, s->seh);
    c->st_top = s->st_top, c->fpu_cw = s->fpu_cw;
}

// A guest lua_CFunction, called by LuaJIT: int __cdecl f(lua_State *L).
static int trampoline(lua_State *L) {
    uint32_t fn = (uint32_t)(uintptr_t)lua_touserdata(L, lua_upvalueindex(1));
    CPU *c = rt_thread_cpu;
    if (!c) { fprintf(stderr, "lua51: guest C function %#x called on a thread without a guest CPU\n", fn); abort(); }
    Saved s __attribute__((cleanup(restore))) = save(c);
    uint32_t arg = gp(L);
    return (int)call_guest(c, fn, 1, &arg);
}

// Mod sandbox.
enum { SB_IO, SB_OS, SB_PACKAGE, SB_DEBUG, SB_FFI, NSB };
static const struct { const char *lib, *open; } SANDBOX[NSB] = {
    {"io", "luaopen_io"}, {"os", "luaopen_os"}, {"package", "luaopen_package"}, {"debug", "luaopen_debug"}, {"ffi", "luaopen_ffi"},
};
static uint32_t SANDBOX_THUNK[NSB], OPENLIBS_THUNK;
static int sandbox_patched(uint32_t thunk) { return rd16(thunk) == 0x05c7 && rd64(thunk + 2) == 0; }  // mov [0], 0

static __attribute__((noreturn)) void sandbox_abort(const char *what) {
    fprintf(stderr, "lua51: %s called while the mod sandbox has patched it out (the original crashes here)\n", what);
    exit(10);
}

static int sandbox_guard(lua_State *L) {
    int lib = (int)(intptr_t)lua_touserdata(L, lua_upvalueindex(2));
    if (sandbox_patched(SANDBOX_THUNK[lib])) {
        char what[128];
        snprintf(what, sizeof what, "LuaJIT %s library function %s", SANDBOX[lib].lib, lua_tostring(L, lua_upvalueindex(3)));
        sandbox_abort(what);
    }
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_call(L, lua_gettop(L) - 1, LUA_MULTRET);
    return lua_gettop(L);
}

// Replace every C function in the table at t with a sandbox_guard closure for library lib.
static void sandbox_wrap(lua_State *L, int t, int lib) {
    if (t < 0) t = lua_gettop(L) + t + 1;
    if (!lua_istable(L, t)) return;
    lua_pushnil(L);
    while (lua_next(L, t)) {  // key, value
        if (lua_iscfunction(L, -1) && lua_type(L, -2) == LUA_TSTRING && lua_tocfunction(L, -1) != sandbox_guard) {
            lua_pushlightuserdata(L, (void *)(intptr_t)lib);
            lua_pushvalue(L, -3);
            lua_pushcclosure(L, sandbox_guard, 3);  // key, guard(value, lib, key)
            lua_pushvalue(L, -2);
            lua_insert(L, -2);
            lua_rawset(L, t);
        } else {
            lua_pop(L, 1);
        }
    }
}

// Wrap the C functions lib keeps outside its own table: file methods, and package's require and module.
static void sandbox_wrap_extras(lua_State *L, int lib) {
    if (lib == SB_IO) {
        lua_getfield(L, LUA_REGISTRYINDEX, LUA_FILEHANDLE);
        sandbox_wrap(L, -1, lib);
        lua_pop(L, 1);
    } else if (lib == SB_PACKAGE) {
        lua_createtable(L, 0, 2);
        for (int i = 0; i < 2; i++) {
            const char *name = i ? "module" : "require";
            lua_getfield(L, LUA_GLOBALSINDEX, name);
            lua_setfield(L, -2, name);
        }
        sandbox_wrap(L, -1, lib);
        for (int i = 0; i < 2; i++) {
            const char *name = i ? "module" : "require";
            lua_getfield(L, -1, name);
            lua_setfield(L, LUA_GLOBALSINDEX, name);
        }
        lua_pop(L, 1);
    }
}

// luaopen_<lib> for a sandboxed library: aborts if it is patched, else opens it (its table is left on
// top) and wraps its C functions.
static int open_lib(lua_State *L, int lib, lua_CFunction open) {
    if (sandbox_patched(SANDBOX_THUNK[lib])) sandbox_abort(SANDBOX[lib].open);
    int n = open(L);
    if (n >= 1) sandbox_wrap(L, -1, lib);
    sandbox_wrap_extras(L, lib);
    return n;
}
static int open_io(lua_State *L) { return open_lib(L, SB_IO, luaopen_io); }
static int open_os(lua_State *L) { return open_lib(L, SB_OS, luaopen_os); }
static int open_package(lua_State *L) { return open_lib(L, SB_PACKAGE, luaopen_package); }
static int open_debug(lua_State *L) { return open_lib(L, SB_DEBUG, luaopen_debug); }
static int open_ffi(lua_State *L) { return open_lib(L, SB_FFI, luaopen_ffi); }

// Host functions for the imported lua51 functions the guest may push as C functions.
static const struct { const char *name; lua_CFunction fn; } OPENERS[] = {
    {"luaopen_base", luaopen_base}, {"luaopen_table", luaopen_table}, {"luaopen_string", luaopen_string},
    {"luaopen_math", luaopen_math}, {"luaopen_bit", luaopen_bit}, {"luaopen_jit", luaopen_jit},
    {"luaopen_io", open_io}, {"luaopen_os", open_os}, {"luaopen_package", open_package},
    {"luaopen_debug", open_debug}, {"luaopen_ffi", open_ffi},
};
enum { NOPENERS = sizeof OPENERS / sizeof *OPENERS };
static uint32_t OPENER_THUNK[NOPENERS];

static pthread_once_t THUNKS_ONCE = PTHREAD_ONCE_INIT;
static void thunks_init(void) {
    for (int i = 0; i < NOPENERS; i++) OPENER_THUNK[i] = rt_thunk("lua51.dll", OPENERS[i].name);
    for (int i = 0; i < NSB; i++) SANDBOX_THUNK[i] = rt_thunk("lua51.dll", SANDBOX[i].open);
    OPENLIBS_THUNK = rt_thunk("lua51.dll", "luaL_openlibs");
}

// State.
HOST_CDECL(lua51, luaL_newstate) {
    pthread_once(&THUNKS_ONCE, thunks_init);
    lua_State *L = lua_newstate(guest_alloc, NULL);
    if (L) lua_atpanic(L, panic);
    ret_i32(c, gp(L));
}
HOST_CDECL(lua51, lua_close) { lua_close(LS(0)); }
HOST_CDECL(lua51, luaL_openlibs) {
    pthread_once(&THUNKS_ONCE, thunks_init);
    if (sandbox_patched(OPENLIBS_THUNK)) sandbox_abort("luaL_openlibs");
    for (int i = 0; i < NSB; i++)
        if (sandbox_patched(SANDBOX_THUNK[i])) sandbox_abort(SANDBOX[i].open);  // luaL_openlibs would open it
    lua_State *L = LS(0);
    luaL_openlibs(L);
    for (int i = 0; i < NSB; i++) {
        lua_getfield(L, LUA_GLOBALSINDEX, SANDBOX[i].lib);
        sandbox_wrap(L, -1, i);
        lua_pop(L, 1);
        sandbox_wrap_extras(L, i);
    }
    lua_getfield(L, LUA_GLOBALSINDEX, "package");  // the ffi loader, for require("ffi")
    lua_getfield(L, -1, "preload");
    if (lua_istable(L, -1)) {
        lua_pushcfunction(L, open_ffi);
        lua_setfield(L, -2, "ffi");
    }
    lua_pop(L, 2);
}
#define OPENER(lib, fn)                                                                               \
    HOST_CDECL(lua51, luaopen_##lib) {                                                                \
        pthread_once(&THUNKS_ONCE, thunks_init);                                                      \
        ret_i32(c, fn(LS(0)));                                                                        \
    }
OPENER(base, luaopen_base)
OPENER(table, luaopen_table)
OPENER(string, luaopen_string)
OPENER(math, luaopen_math)
OPENER(bit, luaopen_bit)
OPENER(jit, luaopen_jit)
OPENER(io, open_io)
OPENER(os, open_os)
OPENER(package, open_package)
OPENER(debug, open_debug)
OPENER(ffi, open_ffi)

// Calls and errors.
HOST_CDECL(lua51, lua_pushcclosure) {
    pthread_once(&THUNKS_ONCE, thunks_init);
    lua_State *L = LS(0);
    uint32_t fn = ARG(1);
    int n = INT(2);
    if (fn >= THUNK_BASE) {
        for (int i = 0; i < NOPENERS; i++)
            if (OPENER_THUNK[i] == fn) {
                lua_pushcclosure(L, OPENERS[i].fn, n);
                return;
            }
        fprintf(stderr, "lua51: lua_pushcclosure of thunk %#x, which has no host lua_CFunction\n", fn);
        exit(4);
    }
    lua_pushlightuserdata(L, (void *)(uintptr_t)fn);
    lua_insert(L, -n - 1);
    lua_pushcclosure(L, trampoline, n + 1);
}
// NOITAMAC_TRACE_LUA=1 logs every chunk the guest loads and every error lua_pcall returns.
static int trace_lua(void) {
    static int t = -1;
    if (t < 0) t = getenv("NOITAMAC_TRACE_LUA") && *getenv("NOITAMAC_TRACE_LUA") != '0';
    return t;
}
static int traced_load(lua_State *L, int r, const char *name, size_t size) {
    if (trace_lua())
        fprintf(stderr, "[lua] %p load %.100s (%zu bytes)%s%s\n", (void *)L, name ? name : "?", size, r ? ": " : "",
                r ? lua_tostring(L, -1) : "");
    return r;
}

HOST_CDECL(lua51, lua_call) { lua_call(LS(0), INT(1), INT(2)); }
HOST_CDECL(lua51, lua_pcall) {
    Saved s = save(c);
    lua_State *L = LS(0);
    int r = lua_pcall(L, INT(1), INT(2), IX(3));
    if (r) restore(&s);
    if (r && trace_lua()) {
        const char *msg = lua_tostring(L, -1);
        fprintf(stderr, "[lua] %p pcall error %d: %s\n", (void *)L, r, msg ? msg : "(not a string)");
    }
    ret_i32(c, r);
}
HOST_CDECL(lua51, lua_error) { lua_error(LS(0)); }
HOST_CDECL(lua51, luaL_loadstring) {
    const char *s = ARG_STR(1);
    ret_i32(c, traced_load(LS(0), luaL_loadstring(LS(0), s), s, strlen(s)));
}
HOST_CDECL(lua51, luaL_loadbufferx) {  // (L, buff, size, name, mode)
    ret_i32(c, traced_load(LS(0), luaL_loadbufferx(LS(0), ARG_STR(1), ARG(2), ARG_STR(3), ARG_STR(4)), ARG_STR(3), ARG(2)));
}
HOST_CDECL(lua51, luaL_loadbuffer) {
    ret_i32(c, traced_load(LS(0), luaL_loadbuffer(LS(0), ARG_STR(1), ARG(2), ARG_STR(3)), ARG_STR(3), ARG(2)));
}

// Stack.
HOST_CDECL(lua51, lua_gettop) { ret_i32(c, lua_gettop(LS(0))); }
HOST_CDECL(lua51, lua_settop) { lua_settop(LS(0), IX(1)); }
HOST_CDECL(lua51, lua_pushvalue) { lua_pushvalue(LS(0), IX(1)); }
HOST_CDECL(lua51, lua_remove) { lua_remove(LS(0), IX(1)); }
HOST_CDECL(lua51, lua_insert) { lua_insert(LS(0), IX(1)); }
HOST_CDECL(lua51, lua_replace) { lua_replace(LS(0), IX(1)); }
HOST_CDECL(lua51, lua_checkstack) { ret_i32(c, lua_checkstack(LS(0), INT(1))); }
HOST_CDECL(lua51, luaL_checkstack) { luaL_checkstack(LS(0), INT(1), ARG_STR(2)); }
HOST_CDECL(lua51, lua_concat) { lua_concat(LS(0), INT(1)); }

// Reading values.
HOST_CDECL(lua51, lua_type) { ret_i32(c, lua_type(LS(0), IX(1))); }
HOST_CDECL(lua51, lua_typename) { ret_i32(c, guest_const(lua_typename(LS(0), INT(1)))); }
HOST_CDECL(lua51, lua_isstring) { ret_i32(c, lua_isstring(LS(0), IX(1))); }
HOST_CDECL(lua51, lua_isnumber) { ret_i32(c, lua_isnumber(LS(0), IX(1))); }
HOST_CDECL(lua51, lua_toboolean) { ret_i32(c, lua_toboolean(LS(0), IX(1))); }
HOST_CDECL(lua51, lua_tonumber) { ret_f64(c, lua_tonumber(LS(0), IX(1))); }
// 32-bit lua_Integer: LuaJIT 2.0 on x86 converts with a truncating cvttsd2si (0x80000000 if out of range).
// The game's x86 lua51.dll converts with lj_num2bit: add 2^52+2^51 and keep the low 32 bits, i.e. round to
// nearest even and wrap modulo 2^32 (spawn-function colors like 0xff6d934c depend on the wrap).
HOST_CDECL(lua51, lua_tointeger) {
    double n = lua_tonumber(LS(0), IX(1)) + 6755399441055744.0;
    uint64_t bits;
    memcpy(&bits, &n, 8);
    ret_i32(c, (uint32_t)bits);
}
HOST_CDECL(lua51, lua_tolstring) {  // (L, idx, size_t *len)
    size_t len = 0;
    const char *s = lua_tolstring(LS(0), IX(1), &len);
    if (ARG(2)) wr32(ARG(2), s ? (uint32_t)len : 0);
    ret_i32(c, gp(s));
}
HOST_CDECL(lua51, lua_objlen) { ret_i32(c, (uint32_t)lua_objlen(LS(0), IX(1))); }
HOST_CDECL(lua51, lua_rawequal) { ret_i32(c, lua_rawequal(LS(0), IX(1), IX(2))); }
// Light userdata is the guest's raw value; everything else is an object in guest memory.
HOST_CDECL(lua51, lua_topointer) {
    lua_State *L = LS(0);
    int i = IX(1);
    const void *p = lua_topointer(L, i);
    ret_i32(c, lua_type(L, i) == LUA_TLIGHTUSERDATA ? (uint32_t)(uintptr_t)p : gp(p));
}
HOST_CDECL(lua51, lua_touserdata) {
    lua_State *L = LS(0);
    int i = IX(1);
    void *p = lua_touserdata(L, i);
    ret_i32(c, lua_type(L, i) == LUA_TLIGHTUSERDATA ? (uint32_t)(uintptr_t)p : gp(p));
}

// Pushing values.
HOST_CDECL(lua51, lua_pushnil) { lua_pushnil(LS(0)); }
HOST_CDECL(lua51, lua_pushnumber) { lua_pushnumber(LS(0), ARG_F64(1)); }
HOST_CDECL(lua51, lua_pushinteger) { lua_pushinteger(LS(0), (int32_t)ARG(1)); }
HOST_CDECL(lua51, lua_pushboolean) { lua_pushboolean(LS(0), INT(1)); }
HOST_CDECL(lua51, lua_pushstring) { lua_pushstring(LS(0), ARG_STR(1)); }
HOST_CDECL(lua51, lua_pushlstring) { lua_pushlstring(LS(0), ARG_STR(1), ARG(2)); }
HOST_CDECL(lua51, lua_pushlightuserdata) { lua_pushlightuserdata(LS(0), (void *)(uintptr_t)ARG(1)); }
HOST_CDECL(lua51, lua_newuserdata) { ret_i32(c, gp(lua_newuserdata(LS(0), ARG(1)))); }

// lua_pushfstring(L, fmt, ...) over guest varargs. Lua's formats: %% %s %d %c %f (a lua_Number) %p.
HOST_CDECL(lua51, lua_pushfstring) {
    lua_State *L = LS(0);
    const char *f = ARG_STR(1);
    uint32_t va = ARG_ADDR(2);
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    for (; *f; f++) {
        char tmp[64];
        if (*f != '%') { luaL_addchar(&b, *f); continue; }
        switch (*++f) {
        case 's': { uint32_t s = rd32(va); va += 4; luaL_addstring(&b, s ? (const char *)P(s) : "(null)"); break; }
        case 'd': snprintf(tmp, sizeof tmp, "%d", (int32_t)rd32(va)), va += 4, luaL_addstring(&b, tmp); break;
        case 'c': luaL_addchar(&b, (char)rd32(va)), va += 4; break;
        case 'f': snprintf(tmp, sizeof tmp, LUA_NUMBER_FMT, rdf64(va)), va += 8, luaL_addstring(&b, tmp); break;
        case 'p': snprintf(tmp, sizeof tmp, "%08X", rd32(va)), va += 4, luaL_addstring(&b, tmp); break;  // MSVC %p
        case '%': luaL_addchar(&b, '%'); break;
        case 0: f--; break;
        default: luaL_addchar(&b, '%'), luaL_addchar(&b, *f); break;
        }
    }
    luaL_pushresult(&b);
    ret_i32(c, gp(lua_tostring(L, -1)));
}

// Tables.
HOST_CDECL(lua51, lua_createtable) { lua_createtable(LS(0), INT(1), INT(2)); }
HOST_CDECL(lua51, lua_getfield) { lua_getfield(LS(0), IX(1), ARG_STR(2)); }
HOST_CDECL(lua51, lua_setfield) { lua_setfield(LS(0), IX(1), ARG_STR(2)); }
HOST_CDECL(lua51, lua_gettable) { lua_gettable(LS(0), IX(1)); }
HOST_CDECL(lua51, lua_settable) { lua_settable(LS(0), IX(1)); }
HOST_CDECL(lua51, lua_rawget) { lua_rawget(LS(0), IX(1)); }
HOST_CDECL(lua51, lua_rawset) { lua_rawset(LS(0), IX(1)); }
HOST_CDECL(lua51, lua_rawgeti) { lua_rawgeti(LS(0), IX(1), INT(2)); }
HOST_CDECL(lua51, lua_rawseti) { lua_rawseti(LS(0), IX(1), INT(2)); }
HOST_CDECL(lua51, lua_next) { ret_i32(c, lua_next(LS(0), IX(1))); }
HOST_CDECL(lua51, lua_getmetatable) { ret_i32(c, lua_getmetatable(LS(0), IX(1))); }
HOST_CDECL(lua51, lua_setmetatable) { ret_i32(c, lua_setmetatable(LS(0), IX(1))); }
HOST_CDECL(lua51, luaL_ref) { ret_i32(c, luaL_ref(LS(0), IX(1))); }
HOST_CDECL(lua51, luaL_unref) { luaL_unref(LS(0), IX(1), INT(2)); }

// Debug interface. The guest's lua_Debug is the 32-bit layout (100 bytes): event, name, namewhat, what,
// source, currentline, nups, linedefined, lastlinedefined, short_src[60], i_ci.
enum { AR_EVENT = 0, AR_NAME = 4, AR_NAMEWHAT = 8, AR_WHAT = 12, AR_SOURCE = 16, AR_CURRENTLINE = 20, AR_NUPS = 24,
       AR_LINEDEFINED = 28, AR_LASTLINEDEFINED = 32, AR_SHORT_SRC = 36, AR_I_CI = 96 };
_Static_assert(LUA_IDSIZE == AR_I_CI - AR_SHORT_SRC, "lua_Debug layout");

HOST_CDECL(lua51, lua_getstack) {  // (L, level, ar)
    lua_Debug ar;
    int r = lua_getstack(LS(0), INT(1), &ar);
    if (r) wr32(ARG(2) + AR_I_CI, (uint32_t)ar.i_ci);
    ret_i32(c, r);
}
HOST_CDECL(lua51, lua_getinfo) {  // (L, what, ar): writes only the fields `what` asks for
    uint32_t g = ARG(2);
    const char *what = ARG_STR(1);
    lua_Debug ar = {.event = (int)rd32(g + AR_EVENT), .i_ci = (int)rd32(g + AR_I_CI)};
    int r = lua_getinfo(LS(0), what, &ar);
    if (strchr(what, 'n')) wr32(g + AR_NAME, guest_const(ar.name)), wr32(g + AR_NAMEWHAT, guest_const(ar.namewhat));
    if (strchr(what, 'S')) {
        wr32(g + AR_WHAT, guest_const(ar.what));
        wr32(g + AR_SOURCE, guest_const(ar.source));
        wr32(g + AR_LINEDEFINED, (uint32_t)ar.linedefined);
        wr32(g + AR_LASTLINEDEFINED, (uint32_t)ar.lastlinedefined);
        memcpy(P(g + AR_SHORT_SRC), ar.short_src, LUA_IDSIZE);
    }
    if (strchr(what, 'l')) wr32(g + AR_CURRENTLINE, (uint32_t)ar.currentline);
    if (strchr(what, 'u')) wr32(g + AR_NUPS, (uint32_t)ar.nups);
    ret_i32(c, r);
}
