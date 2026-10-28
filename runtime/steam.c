// Steamworks (steam_api.dll, built against SDK 1.45/1.46) bridged to the native macOS libsteam_api.dylib.
//
// Loading. SteamAPI_Init dlopens libsteam_api.dylib from $MINA4MAC_STEAM (a directory; "off" disables the bridge)
// or <launcher dir>/steam_api/ (tools/setup_steam.sh installs it there; it must include arm64, so SDK 1.52 or
// later: 1.57 is tested), sets SteamAppId=881100 and calls the native SteamAPI_Init. If the library is missing or
// Steam isn't running, Init fails and the game runs offline, as it does with no Steam at all.
//
// Interfaces. The game gets each interface through SteamInternal_ContextInit, whose init function (guest code)
// calls SteamInternal_FindOrCreateUserInterface(user, version). The bridge asks the native library for the same
// version (the Steam client still serves 2019's interfaces) and hands the game a proxy: a guest object whose
// vtable holds one thunk per method, in MSVC's slot order (runtime/steam_gen.h, from tools/gen_steam.py). The
// methods are __thiscall (`this` in ecx, the callee pops the stack arguments). Implemented methods call the
// native object's method by its declaration index (clang's slot order); calling any other one exits naming it
// ("unimplemented import steam_api.dll!ISteamUGC::..."), so the log says what to add next.
//
// Callbacks. The game registers CCallbackBase objects (vtable, flags byte at +4, callback id at +8; MSVC orders the
// vtable Run(param, ioFailure, call), Run(param), GetCallbackSizeBytes()). The bridge keeps them in a list and
// uses the native library's manual dispatch: SteamAPI_RunCallbacks drains the native queue and calls each matching
// guest Run(param) on the calling thread with the struct converted to the Windows layout (pack 8; macOS packs
// callbacks to 4). Call results (SteamAPI_RegisterCallResult) aren't imported by the game, so they're dropped.
#include <dlfcn.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "heap.h"
#include "hle.h"
#include "host.h"
#include "steam_gen.h"

enum { NOITA_APP_ID = 881100, CALLBACK_FLAGS_REGISTERED = 1, STEAM_API_CALL_COMPLETED = 703, MAX_CALLBACKS = 256 };

typedef struct { int32_t user, id; uint8_t *param; int32_t size; } CallbackMsg;
static struct {
    bool (*Init)(void);
    void (*Shutdown)(void);
    int32_t (*GetHSteamUser)(void);
    int32_t (*GetHSteamPipe)(void);
    void *(*FindOrCreateUserInterface)(int32_t, const char *);
    void (*ManualDispatch_Init)(void);
    void (*ManualDispatch_RunFrame)(int32_t);
    bool (*ManualDispatch_GetNextCallback)(int32_t, CallbackMsg *);
    void (*ManualDispatch_FreeLastCallback)(int32_t);
} S;

static bool ONLINE;
static int32_t PIPE;
static uint32_t COUNTER;  // ContextInit re-runs a context's init function when its counter differs
static void *HOST_OBJ[STEAM_NIFACES];
static uint32_t PROXY[STEAM_NIFACES];
enum { USER, FRIENDS, UTILS, REMOTE_STORAGE, USER_STATS, UGC };  // STEAM_IFACES order
static struct { uint32_t cb; int32_t id; } CBS[MAX_CALLBACKS];
static int NCBS;

static const char *load(void) {
    static char err[PATH_MAX + 64];
    char dir[PATH_MAX], exe[PATH_MAX], path[PATH_MAX];
    const char *env = getenv("MINA4MAC_STEAM");
    if (env && !strcmp(env, "off")) return "disabled (MINA4MAC_STEAM=off)";
    if (env) snprintf(dir, sizeof dir, "%s", env);
    else {
        uint32_t n = sizeof exe;
        if (_NSGetExecutablePath(exe, &n) || !realpath(exe, path)) return "can't find the launcher's directory";
        *strrchr(path, '/') = 0;
        snprintf(dir, sizeof dir, "%s/steam_api", path);
    }
    snprintf(path, sizeof path, "%s/libsteam_api.dylib", dir);
    void *lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!lib) return snprintf(err, sizeof err, "%s", dlerror()), err;
#define SYM(field, name)                                                                                     \
    if (!(*(void **)&S.field = dlsym(lib, name))) return snprintf(err, sizeof err, "%s has no %s", path, name), err;
    SYM(Init, "SteamAPI_Init")
    SYM(Shutdown, "SteamAPI_Shutdown")
    SYM(GetHSteamUser, "SteamAPI_GetHSteamUser")
    SYM(GetHSteamPipe, "SteamAPI_GetHSteamPipe")
    SYM(FindOrCreateUserInterface, "SteamInternal_FindOrCreateUserInterface")
    SYM(ManualDispatch_Init, "SteamAPI_ManualDispatch_Init")
    SYM(ManualDispatch_RunFrame, "SteamAPI_ManualDispatch_RunFrame")
    SYM(ManualDispatch_GetNextCallback, "SteamAPI_ManualDispatch_GetNextCallback")
    SYM(ManualDispatch_FreeLastCallback, "SteamAPI_ManualDispatch_FreeLastCallback")
#undef SYM
    return NULL;
}

HOST_CDECL(steam_api, SteamAPI_RestartAppIfNecessary) { ret_i32(c, 0); }  // never relaunch the Windows game

HOST_CDECL(steam_api, SteamAPI_Init) {
    if (!ONLINE) {
        static const char *err;
        static bool tried;
        if (!tried) tried = true, err = load();
        if (err) fprintf(stderr, "[steam] offline: %s\n", err);
        else {
            char id[16];
            snprintf(id, sizeof id, "%d", NOITA_APP_ID);
            // Overwrite: Steam sets these to a shortcut's own id when it launches the app as a non-Steam game.
            setenv("SteamAppId", id, 1);
            setenv("SteamGameId", id, 1);
            if (!S.Init()) fprintf(stderr, "[steam] offline: SteamAPI_Init failed (is Steam running and logged in?)\n");
            else {
                S.ManualDispatch_Init();
                PIPE = S.GetHSteamPipe();
                ONLINE = true;
                COUNTER++;
                fprintf(stderr, "[steam] online (app %d)\n", NOITA_APP_ID);
            }
        }
    }
    ret_i32(c, ONLINE);
}

HOST_CDECL(steam_api, SteamAPI_Shutdown) {
    if (!ONLINE) return;
    S.Shutdown();
    ONLINE = false;
    COUNTER++;
    memset(HOST_OBJ, 0, sizeof HOST_OBJ);
}

HOST_CDECL(steam_api, SteamAPI_GetHSteamUser) { ret_i32(c, ONLINE ? S.GetHSteamUser() : 0); }

// ContextInit(ctx), ctx = {init function, counter, interface pointer}: returns &ctx[2], first running the
// (cdecl) init function on it if the counter changed since. Offline, the counter is 0, as in a fresh context,
// so the pointer stays NULL.
HOST_CDECL(steam_api, SteamInternal_ContextInit) {
    uint32_t ctx = ARG(0);
    if (rd32(ctx + 4) != COUNTER) {
        wr32(ctx + 4, COUNTER);
        wr32(ctx + 8, 0);
        if (ONLINE) call_guest(c, rd32(ctx), 1, (uint32_t[]){ctx + 8});
    }
    ret_i32(c, ctx + 8);
}

HOST_CDECL(steam_api, SteamInternal_FindOrCreateUserInterface) {
    int32_t user = (int32_t)ARG(0);
    const char *ver = ARG_STR(1);
    int i = 0;
    while (i < STEAM_NIFACES && strcmp(STEAM_IFACES[i].version, ver)) i++;
    if (!ONLINE || i == STEAM_NIFACES) {
        if (ONLINE) fprintf(stderr, "[steam] no bridge for interface %s\n", ver);
        ret_i32(c, 0);
        return;
    }
    if (!HOST_OBJ[i] && !(HOST_OBJ[i] = S.FindOrCreateUserInterface(user, ver))) {
        fprintf(stderr, "[steam] Steam has no interface %s\n", ver);
        ret_i32(c, 0);
        return;
    }
    if (!PROXY[i]) {
        const SteamIface *f = &STEAM_IFACES[i];
        uint32_t vt = heap_alloc(4 * f->n), obj = heap_calloc(1, 16);
        for (int k = 0; k < f->n; k++) {
            char name[128];
            snprintf(name, sizeof name, "%s::%s", f->cls, f->m[k].name);
            wr32(vt + 4 * k, rt_thunk("steam_api.dll", name));
        }
        wr32(obj, vt);
        PROXY[i] = obj;
    }
    ret_i32(c, PROXY[i]);
}

// Callbacks.
HOST_CDECL(steam_api, SteamAPI_RegisterCallback) {
    uint32_t cb = ARG(0);
    int32_t id = (int32_t)ARG(1);
    wr8(cb + 4, rd8(cb + 4) | CALLBACK_FLAGS_REGISTERED);
    wr32(cb + 8, id);
    for (int i = 0; i < NCBS; i++)
        if (CBS[i].cb == cb) { CBS[i].id = id; return; }
    if (NCBS == MAX_CALLBACKS) { fprintf(stderr, "[steam] too many callbacks\n"); exit(2); }
    CBS[NCBS++] = (typeof(*CBS)){cb, id};
}

HOST_CDECL(steam_api, SteamAPI_UnregisterCallback) {
    uint32_t cb = ARG(0);
    wr8(cb + 4, rd8(cb + 4) & ~CALLBACK_FLAGS_REGISTERED);
    for (int i = 0; i < NCBS; i++)
        if (CBS[i].cb == cb) { CBS[i] = CBS[--NCBS]; break; }
}

static const SteamCallback *callback_layout(int32_t id) {
    for (int i = 0; i < STEAM_NCALLBACKS; i++)
        if (STEAM_CALLBACKS[i].id == id) return &STEAM_CALLBACKS[i];
    return NULL;
}

static void dispatch(CPU *c, const CallbackMsg *m) {
    int n = 0;
    for (int i = 0; i < NCBS; i++) n += CBS[i].id == m->id;
    if (!n) return;
    const SteamCallback *d = callback_layout(m->id);
    if (!d || d->n < 0 || m->size < d->mac_size) {
        static int32_t warned[64];
        for (int i = 0; i < 64; i++) {
            if (warned[i] == m->id) return;
            if (!warned[i]) { warned[i] = m->id; break; }
        }
        fprintf(stderr, "[steam] callback %d (%s, %d bytes): can't convert it, not delivered\n", m->id,
                d ? d->name : "unknown", m->size);
        return;
    }
    uint32_t g = heap_calloc(1, d->win_size);
    for (int k = 0; k < d->n; k++) memcpy(P(g + d->f[k].win), m->param + d->f[k].mac, d->f[k].size);
    for (int i = 0; i < NCBS; i++) {
        if (CBS[i].id != m->id) continue;
        uint32_t cb = CBS[i].cb;
        // The game's GetCallbackSizeBytes (vtable slot 2) compiles to `mov eax, sizeof(param); ret`. Its original
        // x86 bytes are still mapped, so they check both the vtable order and the generated Windows layout.
        uint32_t size_fn = rd32(rd32(cb) + 8);
        if (rd8(size_fn) == 0xb8 && rd8(size_fn + 5) == 0xc3 && rd32(size_fn + 1) != (uint32_t)d->win_size) {
            fprintf(stderr, "[steam] callback %d (%s): the game expects %u bytes, the layout has %d; not delivered\n",
                    m->id, d->name, rd32(size_fn + 1), d->win_size);
            continue;
        }
        c->ecx = cb;  // __thiscall Run(param): vtable slot 1
        call_guest(c, rd32(rd32(cb) + 4), 1, (uint32_t[]){g});
        if (i < NCBS && CBS[i].cb != cb) i--;  // the callback unregistered itself
    }
    heap_free(g);
}

HOST_CDECL(steam_api, SteamAPI_RunCallbacks) {
    if (!ONLINE) return;
    S.ManualDispatch_RunFrame(PIPE);
    CallbackMsg m;
    while (S.ManualDispatch_GetNextCallback(PIPE, &m)) {
        if (m.id != STEAM_API_CALL_COMPLETED) dispatch(c, &m);
        S.ManualDispatch_FreeLastCallback(PIPE);
    }
}

// Interface methods. M(iface, method, argbytes) declares the host implementation of the proxy's thunk
// "steam_api.dll!<class>::<method>"; NATIVE(i, method, type) is the native object's method (call it with
// HOST_OBJ[i] first).
#define M(cls, meth, argbytes) HOST(steam_api, cls##_##meth, #cls "::" #meth, argbytes)
#define NATIVE(i, cls, meth, ret, ...) ((ret(*)(void *, ##__VA_ARGS__))(*(void ***)HOST_OBJ[i])[cls##_##meth])
#define B(n) ((ARG(n) & 0xff) != 0)  // a bool argument (one stack slot, low byte)

// A string Steam returns, as a guest string. Kept (and reused while unchanged): the game may hold on to it.
static uint32_t guest_string(uint32_t *cache, const char *s) {
    if (!s) return 0;
    if (!*cache || strcmp((char *)P(*cache), s)) *cache = guest_strdup(s);
    return *cache;
}

// ISteamUser020
M(ISteamUser, GetHSteamUser, 0) { ret_i32(c, NATIVE(USER, ISteamUser, GetHSteamUser, int32_t)(HOST_OBJ[USER])); }
M(ISteamUser, BLoggedOn, 0) { ret_i32(c, NATIVE(USER, ISteamUser, BLoggedOn, bool)(HOST_OBJ[USER])); }
M(ISteamUser, GetSteamID, 4) {  // returns a class: MSVC passes a hidden result pointer (and returns it)
    wr64(ARG(0), NATIVE(USER, ISteamUser, GetSteamID, uint64_t)(HOST_OBJ[USER]));
    ret_i32(c, ARG(0));
}
M(ISteamUser, GetPlayerSteamLevel, 0) {
    ret_i32(c, NATIVE(USER, ISteamUser, GetPlayerSteamLevel, int32_t)(HOST_OBJ[USER]));
}

// ISteamFriends017
M(ISteamFriends, GetPersonaName, 0) {
    static uint32_t s;
    ret_i32(c, guest_string(&s, NATIVE(FRIENDS, ISteamFriends, GetPersonaName, const char *)(HOST_OBJ[FRIENDS])));
}
M(ISteamFriends, ActivateGameOverlay, 4) {
    NATIVE(FRIENDS, ISteamFriends, ActivateGameOverlay, void, const char *)(HOST_OBJ[FRIENDS], ARG_STR(0));
}
M(ISteamFriends, ActivateGameOverlayToWebPage, 8) {
    NATIVE(FRIENDS, ISteamFriends, ActivateGameOverlayToWebPage, void, const char *, int32_t)(HOST_OBJ[FRIENDS],
                                                                                             ARG_STR(0), ARG(1));
}
M(ISteamFriends, ActivateGameOverlayToStore, 8) {
    NATIVE(FRIENDS, ISteamFriends, ActivateGameOverlayToStore, void, uint32_t, int32_t)(HOST_OBJ[FRIENDS], ARG(0),
                                                                                       ARG(1));
}
M(ISteamFriends, SetRichPresence, 8) {
    ret_i32(c, NATIVE(FRIENDS, ISteamFriends, SetRichPresence, bool, const char *, const char *)(
                   HOST_OBJ[FRIENDS], ARG_STR(0), ARG_STR(1)));
}
M(ISteamFriends, ClearRichPresence, 0) {
    NATIVE(FRIENDS, ISteamFriends, ClearRichPresence, void)(HOST_OBJ[FRIENDS]);
}

// ISteamUtils009
M(ISteamUtils, GetAppID, 0) { ret_i32(c, NATIVE(UTILS, ISteamUtils, GetAppID, uint32_t)(HOST_OBJ[UTILS])); }
M(ISteamUtils, IsOverlayEnabled, 0) {
    ret_i32(c, NATIVE(UTILS, ISteamUtils, IsOverlayEnabled, bool)(HOST_OBJ[UTILS]));
}
M(ISteamUtils, BOverlayNeedsPresent, 0) {
    ret_i32(c, NATIVE(UTILS, ISteamUtils, BOverlayNeedsPresent, bool)(HOST_OBJ[UTILS]));
}
M(ISteamUtils, GetSecondsSinceAppActive, 0) {
    ret_i32(c, NATIVE(UTILS, ISteamUtils, GetSecondsSinceAppActive, uint32_t)(HOST_OBJ[UTILS]));
}
M(ISteamUtils, GetServerRealTime, 0) {
    ret_i32(c, NATIVE(UTILS, ISteamUtils, GetServerRealTime, uint32_t)(HOST_OBJ[UTILS]));
}
M(ISteamUtils, GetConnectedUniverse, 0) {
    ret_i32(c, NATIVE(UTILS, ISteamUtils, GetConnectedUniverse, int32_t)(HOST_OBJ[UTILS]));
}
M(ISteamUtils, GetSteamUILanguage, 0) {
    static uint32_t s;
    ret_i32(c, guest_string(&s, NATIVE(UTILS, ISteamUtils, GetSteamUILanguage, const char *)(HOST_OBJ[UTILS])));
}
M(ISteamUtils, SetOverlayNotificationPosition, 4) {
    NATIVE(UTILS, ISteamUtils, SetOverlayNotificationPosition, void, int32_t)(HOST_OBJ[UTILS], ARG(0));
}

// ISteamRemoteStorage014: the game only checks and sets whether cloud saves are on (they're Steam Auto-Cloud).
M(ISteamRemoteStorage, IsCloudEnabledForAccount, 0) {
    ret_i32(c, NATIVE(REMOTE_STORAGE, ISteamRemoteStorage, IsCloudEnabledForAccount, bool)(HOST_OBJ[REMOTE_STORAGE]));
}
M(ISteamRemoteStorage, IsCloudEnabledForApp, 0) {
    ret_i32(c, NATIVE(REMOTE_STORAGE, ISteamRemoteStorage, IsCloudEnabledForApp, bool)(HOST_OBJ[REMOTE_STORAGE]));
}
M(ISteamRemoteStorage, SetCloudEnabledForApp, 4) {
    NATIVE(REMOTE_STORAGE, ISteamRemoteStorage, SetCloudEnabledForApp, void, bool)(HOST_OBJ[REMOTE_STORAGE], B(0));
}

// ISteamUserStats011. Overloads: _0 is the int32 one, _1 the float one (declaration order).
M(ISteamUserStats, RequestCurrentStats, 0) {
    ret_i32(c, NATIVE(USER_STATS, ISteamUserStats, RequestCurrentStats, bool)(HOST_OBJ[USER_STATS]));
}
M(ISteamUserStats, GetStat_0, 8) {
    ret_i32(c, NATIVE(USER_STATS, ISteamUserStats, GetStat_0, bool, const char *, int32_t *)(
                   HOST_OBJ[USER_STATS], ARG_STR(0), ARG_PTR(1)));
}
M(ISteamUserStats, GetStat_1, 8) {
    ret_i32(c, NATIVE(USER_STATS, ISteamUserStats, GetStat_1, bool, const char *, float *)(
                   HOST_OBJ[USER_STATS], ARG_STR(0), ARG_PTR(1)));
}
M(ISteamUserStats, SetStat_0, 8) {
    ret_i32(c, NATIVE(USER_STATS, ISteamUserStats, SetStat_0, bool, const char *, int32_t)(
                   HOST_OBJ[USER_STATS], ARG_STR(0), (int32_t)ARG(1)));
}
M(ISteamUserStats, SetStat_1, 8) {
    ret_i32(c, NATIVE(USER_STATS, ISteamUserStats, SetStat_1, bool, const char *, float)(
                   HOST_OBJ[USER_STATS], ARG_STR(0), ARG_F32(1)));
}
M(ISteamUserStats, GetAchievement, 8) {
    ret_i32(c, NATIVE(USER_STATS, ISteamUserStats, GetAchievement, bool, const char *, bool *)(
                   HOST_OBJ[USER_STATS], ARG_STR(0), ARG_PTR(1)));
}
M(ISteamUserStats, SetAchievement, 4) {
    const char *name = ARG_STR(0);
    bool ok = NATIVE(USER_STATS, ISteamUserStats, SetAchievement, bool, const char *)(HOST_OBJ[USER_STATS], name);
    fprintf(stderr, "[steam] SetAchievement(%s): %s\n", name, ok ? "ok" : "failed");
    ret_i32(c, ok);
}
M(ISteamUserStats, ClearAchievement, 4) {
    ret_i32(c, NATIVE(USER_STATS, ISteamUserStats, ClearAchievement, bool, const char *)(HOST_OBJ[USER_STATS],
                                                                                         ARG_STR(0)));
}
M(ISteamUserStats, StoreStats, 0) {
    ret_i32(c, NATIVE(USER_STATS, ISteamUserStats, StoreStats, bool)(HOST_OBJ[USER_STATS]));
}
M(ISteamUserStats, IndicateAchievementProgress, 12) {
    ret_i32(c, NATIVE(USER_STATS, ISteamUserStats, IndicateAchievementProgress, bool, const char *, uint32_t, uint32_t)(
                   HOST_OBJ[USER_STATS], ARG_STR(0), ARG(1), ARG(2)));
}
M(ISteamUserStats, GetNumAchievements, 0) {
    ret_i32(c, NATIVE(USER_STATS, ISteamUserStats, GetNumAchievements, uint32_t)(HOST_OBJ[USER_STATS]));
}

// ISteamUGC013: subscribed Workshop items. A PublishedFileId_t argument takes two stack slots.
M(ISteamUGC, GetNumSubscribedItems, 0) {
    ret_i32(c, NATIVE(UGC, ISteamUGC, GetNumSubscribedItems, uint32_t)(HOST_OBJ[UGC]));
}
M(ISteamUGC, GetSubscribedItems, 8) {
    ret_i32(c, NATIVE(UGC, ISteamUGC, GetSubscribedItems, uint32_t, uint64_t *, uint32_t)(HOST_OBJ[UGC], ARG_PTR(0),
                                                                                          ARG(1)));
}
M(ISteamUGC, GetItemState, 8) {
    ret_i32(c, NATIVE(UGC, ISteamUGC, GetItemState, uint32_t, uint64_t)(HOST_OBJ[UGC], ARG_I64(0)));
}
M(ISteamUGC, GetItemInstallInfo, 24) {  // (id, uint64 *size, char *folder, uint32 folder size, uint32 *time)
    char host[PATH_MAX];
    uint32_t folder = ARG(3), cap = ARG(4);
    bool ok = NATIVE(UGC, ISteamUGC, GetItemInstallInfo, bool, uint64_t, uint64_t *, char *, uint32_t, uint32_t *)(
        HOST_OBJ[UGC], ARG_I64(0), ARG_PTR(2), host, sizeof host, ARG_PTR(5));
    // The game opens the folder through the Windows file APIs: give it as a Z: path (hle.h).
    if (ok && folder && cap && !win_path(host, (char *)P(folder), cap)) {
        fprintf(stderr, "[steam] Workshop folder too long for the game's %u-byte buffer: %s\n", cap, host);
        ok = false;
    }
    ret_i32(c, ok);
}
M(ISteamUGC, GetItemDownloadInfo, 16) {
    ret_i32(c, NATIVE(UGC, ISteamUGC, GetItemDownloadInfo, bool, uint64_t, uint64_t *, uint64_t *)(
                   HOST_OBJ[UGC], ARG_I64(0), ARG_PTR(2), ARG_PTR(3)));
}
M(ISteamUGC, DownloadItem, 12) {
    ret_i32(c, NATIVE(UGC, ISteamUGC, DownloadItem, bool, uint64_t, bool)(HOST_OBJ[UGC], ARG_I64(0), B(2)));
}

__attribute__((constructor)) static void check_order(void) {  // the enum above must follow STEAM_IFACES
    static const char *const CLS[] = {"ISteamUser", "ISteamFriends", "ISteamUtils", "ISteamRemoteStorage",
                                      "ISteamUserStats", "ISteamUGC"};
    for (int i = 0; i < STEAM_NIFACES; i++)
        if (strcmp(STEAM_IFACES[i].cls, CLS[i])) { fprintf(stderr, "steam.c: STEAM_IFACES order changed\n"); exit(2); }
}
