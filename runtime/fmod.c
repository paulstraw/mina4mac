// FMOD Studio 2.01 (fmodstudio.dll, fmod.dll) bridged to the native macOS FMOD Engine. The 42 imports are
// C++ member functions with __stdcall, so `this` is the first stack argument. Each one calls the matching
// C API function through a backend table (fmod.h):
//   - NOITAMAC_AUDIO=fmod: the native libfmod/libfmodstudio dylibs, dlopened from $NOITAMAC_FMOD or
//     <launcher dir>/fmod_api/lib (tools/setup_fmod.sh installs them there). Exits if they don't load.
//   - NOITAMAC_AUDIO=stub: the silent backend (fmod_stub.c).
//   - unset: the dylibs if they load, otherwise the stub.
// NOITAMAC_AUDIO_WAV=<file> records the mix to a WAV file (FMOD's WAV writer output) instead of playing it.
// The game's 2.01.05 banks and header version work with the 2.01.23 runtime.
//
// Handles. The guest sees 32-bit handles {generation:8, slot:20, kind:4} into a host table holding the host
// object, so a stale handle (a destroyed instance) is invalid rather than aliasing a new one. Every host
// object but an event instance has one handle for its lifetime (the game keys maps by description handle),
// found through a host-pointer map. An instance's host user data holds its handle; the guest's user data
// lives in the table. An instance handle is retired when FMOD destroys the instance (the DESTROYED
// callback, which the bridge always subscribes to), as a real FMOD handle stops being valid then.
//
// Callbacks. FMOD calls the bridge's host callbacks, which call the game's __stdcall callbacks on the
// calling thread's guest CPU. That is usually FMOD's Studio update thread, which gets a guest thread (stack
// and TEB) on its first callback, as on Windows, where the game's callbacks also run on FMOD's threads.
// Studio's threads are given host stacks as big as guest threads' (thread.c). Callback parameters the game
// can read are copied onto the guest stack: timeline markers (with the name), beats and nested beats; the
// system callback's BANK_UNLOAD bank becomes its handle. Other types get NULL parameters (the game only
// reads markers and beats, 0x47d0e0).
#include <dlfcn.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <os/lock.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fmod.h"
#include "hle.h"
#include "host.h"
#include "proc.h"

#ifdef FMOD_REAL_HEADERS  // every table entry has the SDK's prototype
#define FMOD_API_CHECK(ret, name, params) \
    _Static_assert(__builtin_types_compatible_p(__typeof__(&FMOD_##name), ret(*) params), #name);
FMOD_API(FMOD_API_CHECK)
#endif

enum { K_SYSTEM = 1, K_CORE, K_CHANNELGROUP, K_BANK, K_BUS, K_DESC, K_INSTANCE };
enum { SLOT_BITS = 20, SLOTS = 1 << SLOT_BITS };
enum { HOST_STACK = 64 << 20 };  // as thread.c: recompiled code keeps its C frames on the host stack

static const FmodApi *F;

// Handle table.
typedef struct { void *host; uint32_t handle, user, cb, mask, next_free; } Entry;
static Entry *TAB;
static uint32_t NEXT_SLOT = 1, FREE_SLOT;
static os_unfair_lock LOCK = OS_UNFAIR_LOCK_INIT;

static uint32_t handle_new(uint32_t kind, void *host) {
    os_unfair_lock_lock(&LOCK);
    if (!TAB) TAB = calloc(SLOTS, sizeof *TAB);
    uint32_t n = FREE_SLOT;
    if (n) FREE_SLOT = TAB[n].next_free;
    else if (NEXT_SLOT < SLOTS) n = NEXT_SLOT++;
    else { fprintf(stderr, "fmod: out of handles\n"); exit(2); }
    Entry *e = &TAB[n];
    uint32_t gen = ((e->handle >> 24) + 1) & 0xff;
    *e = (Entry){.host = host, .handle = gen << 24 | n << 4 | kind};
    os_unfair_lock_unlock(&LOCK);
    return e->handle;
}
static Entry *entry(uint32_t h, uint32_t kind) {
    uint32_t n = h >> 4 & (SLOTS - 1);
    if (!TAB || (h & 15) != kind || !n) return NULL;
    Entry *e = &TAB[n];
    return __atomic_load_n(&e->handle, __ATOMIC_ACQUIRE) == h ? e : NULL;
}
static void *host_of(uint32_t h, uint32_t kind) {
    Entry *e = entry(h, kind);
    return e ? e->host : NULL;
}
static void handle_retire(uint32_t h, uint32_t kind) {
    os_unfair_lock_lock(&LOCK);
    Entry *e = entry(h, kind);
    if (e) {
        uint32_t n = e - TAB;
        e->host = NULL, e->next_free = FREE_SLOT, FREE_SLOT = n;
        __atomic_store_n(&e->handle, e->handle & 0xff000000u, __ATOMIC_RELEASE);  // keeps the generation
    }
    os_unfair_lock_unlock(&LOCK);
}

// The one handle of a host object that isn't an event instance (0 for NULL).
typedef struct Wrap { struct Wrap *next; void *host; uint32_t handle; } Wrap;
static Wrap *WRAPS[4096];
static os_unfair_lock WRAP_LOCK = OS_UNFAIR_LOCK_INIT;
static uint32_t wrap(uint32_t kind, void *host) {
    if (!host) return 0;
    Wrap **b = &WRAPS[((uintptr_t)host >> 4) * 0x9e3779b1u % 4096], *w;
    os_unfair_lock_lock(&WRAP_LOCK);
    for (w = *b; w && (w->host != host || (w->handle & 15) != kind); w = w->next) {}
    if (!w) {
        w = malloc(sizeof *w);
        *w = (Wrap){.next = *b, .host = host, .handle = handle_new(kind, host)};
        *b = w;
    }
    os_unfair_lock_unlock(&WRAP_LOCK);
    return w->handle;
}

// A guest copy of a host string, one per distinct string, kept for the process lifetime.
typedef struct Interned { struct Interned *next; uint32_t g; char s[]; } Interned;
static Interned *STRS[1024];
static uint32_t intern(const char *s) {
    if (!s) return 0;
    uint32_t hash = 0;
    for (const char *p = s; *p; p++) hash = hash * 31 + (uint8_t)*p;
    Interned **b = &STRS[hash % 1024], *n;
    os_unfair_lock_lock(&WRAP_LOCK);
    for (n = *b; n && strcmp(n->s, s); n = n->next) {}
    if (!n) {
        n = malloc(sizeof *n + strlen(s) + 1);
        n->next = *b, n->g = guest_strdup(s);
        strcpy(n->s, s);
        *b = n;
    }
    os_unfair_lock_unlock(&WRAP_LOCK);
    return n->g;
}

// Backend selection (at Studio::System::create, the game's first FMOD call).
static FmodApi NATIVE;
static const char *load_native(void) {
    static char err[PATH_MAX + 256];
    char dir[PATH_MAX], exe[PATH_MAX], path[PATH_MAX];
    const char *env = getenv("NOITAMAC_FMOD");
    if (env) snprintf(dir, sizeof dir, "%s", env);
    else {
        uint32_t n = sizeof exe;
        if (_NSGetExecutablePath(exe, &n) || !realpath(exe, path)) return "can't find the launcher's directory";
        *strrchr(path, '/') = 0;
        snprintf(dir, sizeof dir, "%s/fmod_api/lib", path);
    }
    void *libs[2];
    const char *names[2] = {"libfmod.dylib", "libfmodstudio.dylib"};  // core first: studio links @rpath/libfmod.dylib
    for (int i = 0; i < 2; i++) {
        snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        if (!(libs[i] = dlopen(path, RTLD_NOW | RTLD_GLOBAL))) {
            snprintf(err, sizeof err, "%s", dlerror());
            return err;
        }
    }
#define FMOD_API_SYM(ret, name, params)                                                                \
    if (!(*(void **)&NATIVE.name = dlsym(RTLD_DEFAULT, "FMOD_" #name))) {                              \
        snprintf(err, sizeof err, "%s has no FMOD_%s", dir, #name);                                    \
        return err;                                                                                    \
    }
    FMOD_API(FMOD_API_SYM)
#undef FMOD_API_SYM
    return NULL;
}
static void select_backend(void) {
    if (F) return;
    const char *mode = getenv("NOITAMAC_AUDIO");
    if (mode && strcmp(mode, "stub") && strcmp(mode, "fmod")) {
        fprintf(stderr, "[noitamac] NOITAMAC_AUDIO=%s: expected fmod or stub\n", mode);
        mode = NULL;
    }
    if (mode && !strcmp(mode, "stub")) { F = &FMOD_STUB; return; }
    const char *err = load_native();
    if (!err) {
        F = &NATIVE;
        // Studio's threads run the game's callbacks (recompiled code), so give them guest-thread-sized stacks.
        for (int t = FM_THREAD_TYPE_STUDIO_UPDATE; t <= FM_THREAD_TYPE_STUDIO_LOAD_SAMPLE; t++)
            F->Thread_SetAttributes(t, FM_THREAD_AFFINITY_GROUP_DEFAULT, FM_THREAD_PRIORITY_DEFAULT, HOST_STACK);
        return;
    }
    if (mode) { fprintf(stderr, "[noitamac] NOITAMAC_AUDIO=fmod: %s\n", err); exit(2); }
    fprintf(stderr, "[noitamac] no FMOD (%s; run tools/setup_fmod.sh): audio is silent\n", err);
    F = &FMOD_STUB;
}

// Callbacks into the guest.
static CPU *callback_cpu(void) {
    if (!rt_thread_cpu) {  // a host thread of FMOD's: give it a guest thread
        CPU *c = calloc(1, sizeof *c);
        c->fpu_cw = 0x27f;
        rt_thread_init(c);
    }
    return rt_thread_cpu;
}
static uint32_t stack_copy(CPU *c, const void *p, uint32_t n) {  // below esp, 16-byte aligned
    c->esp = (c->esp - n) & ~15u;
    memcpy(P(c->esp), p, n);
    return c->esp;
}

static FMOD_RESULT event_callback(FMOD_STUDIO_EVENT_CALLBACK_TYPE type, FMOD_STUDIO_EVENTINSTANCE *ev, void *params) {
    void *u = NULL;
    F->Studio_EventInstance_GetUserData(ev, &u);
    uint32_t h = (uint32_t)(uintptr_t)u;
    Entry *e = entry(h, K_INSTANCE);
    FMOD_RESULT r = FM_OK;
    if (e && e->cb && e->mask & type) {
        CPU *c = callback_cpu();
        uint32_t sp = c->esp, gp = 0;
        if (params && type == FM_EVENT_CALLBACK_TIMELINE_MARKER) {
            const FMOD_STUDIO_TIMELINE_MARKER_PROPERTIES *m = params;
            uint32_t name = stack_copy(c, m->name, strlen(m->name) + 1);
            gp = stack_copy(c, (uint32_t[]){name, (uint32_t)m->position}, 8);
        } else if (params && type == FM_EVENT_CALLBACK_TIMELINE_BEAT) {
            gp = stack_copy(c, params, 24);  // {bar, beat, position, tempo, time signature upper, lower}
        } else if (params && type == FM_EVENT_CALLBACK_NESTED_TIMELINE_BEAT) {
            gp = stack_copy(c, params, 40);  // {event GUID, beat properties}
        }
        r = (FMOD_RESULT)call_guest(c, e->cb, 3, (uint32_t[]){type, h, gp});
        c->esp = sp;
    }
    if (type == FM_EVENT_CALLBACK_DESTROYED) handle_retire(h, K_INSTANCE);
    return r;
}

static uint32_t SYS_CB;  // the game's system callback (one Studio system)
static FMOD_RESULT system_callback(FMOD_STUDIO_SYSTEM *sys, FMOD_STUDIO_SYSTEM_CALLBACK_TYPE type, void *data, void *user) {
    (void)user;
    uint32_t cb = __atomic_load_n(&SYS_CB, __ATOMIC_ACQUIRE);
    if (!cb) return FM_OK;
    uint32_t gdata = type == FM_SYSTEM_CALLBACK_BANK_UNLOAD ? wrap(K_BANK, data) : 0;
    return (FMOD_RESULT)call_guest(callback_cpu(), cb, 4, (uint32_t[]){wrap(K_SYSTEM, sys), type, gdata, 0});
}

// Import bodies: the host object of argument n (or return FMOD_ERR_INVALID_HANDLE), guest pointers.
#define OBJ(type, kind, n)                                                  \
    type *o = host_of(ARG(n), kind);                                        \
    if (!o) { ret_i32(c, FM_ERR_INVALID_HANDLE); return; }
#define GP(n) host_ptr(ARG(n))
#define RET(call) ret_i32(c, (uint32_t)(call))
static void out32(uint32_t p, uint32_t v) { if (p) wr32(p, v); }

// Core (fmod.dll).
HOST(fmod, getMasterChannelGroup, "?getMasterChannelGroup@System@FMOD@@QAG?AW4FMOD_RESULT@@PAPAVChannelGroup@2@@Z", 8) {
    OBJ(FMOD_SYSTEM, K_CORE, 0);
    FMOD_CHANNELGROUP *g = NULL;
    FMOD_RESULT r = F->System_GetMasterChannelGroup(o, &g);
    out32(ARG(1), wrap(K_CHANNELGROUP, g)), RET(r);
}
HOST(fmod, getVersion, "?getVersion@System@FMOD@@QAG?AW4FMOD_RESULT@@PAI@Z", 8) { OBJ(FMOD_SYSTEM, K_CORE, 0); RET(F->System_GetVersion(o, GP(1))); }
HOST(fmod, set3DSettings, "?set3DSettings@System@FMOD@@QAG?AW4FMOD_RESULT@@MMM@Z", 16) {
    OBJ(FMOD_SYSTEM, K_CORE, 0);
    RET(F->System_Set3DSettings(o, ARG_F32(1), ARG_F32(2), ARG_F32(3)));
}
HOST(fmod, setDSPBufferSize, "?setDSPBufferSize@System@FMOD@@QAG?AW4FMOD_RESULT@@IH@Z", 12) {
    OBJ(FMOD_SYSTEM, K_CORE, 0);
    RET(F->System_SetDSPBufferSize(o, ARG(1), (int32_t)ARG(2)));
}
HOST(fmod, setOutput, "?setOutput@System@FMOD@@QAG?AW4FMOD_RESULT@@W4FMOD_OUTPUTTYPE@@@Z", 8) {
    OBJ(FMOD_SYSTEM, K_CORE, 0);
    RET(F->System_SetOutput(o, (FMOD_OUTPUTTYPE)ARG(1)));
}
HOST(fmod, setSoftwareChannels, "?setSoftwareChannels@System@FMOD@@QAG?AW4FMOD_RESULT@@H@Z", 8) {
    OBJ(FMOD_SYSTEM, K_CORE, 0);
    RET(F->System_SetSoftwareChannels(o, (int32_t)ARG(1)));
}
HOST(fmod, cc_setVolume, "?setVolume@ChannelControl@FMOD@@QAG?AW4FMOD_RESULT@@M@Z", 8) {  // on the master channel group
    OBJ(FMOD_CHANNELGROUP, K_CHANNELGROUP, 0);
    RET(F->ChannelGroup_SetVolume(o, ARG_F32(1)));
}

// Studio::System.
HOST(fmodstudio, create, "?create@System@Studio@FMOD@@SG?AW4FMOD_RESULT@@PAPAV123@I@Z", 8) {
    select_backend();
    FMOD_STUDIO_SYSTEM *s = NULL;
    FMOD_RESULT r = F->Studio_System_Create(&s, ARG(1));
    out32(ARG(0), wrap(K_SYSTEM, s)), RET(r);
}
HOST(fmodstudio, sys_initialize, "?initialize@System@Studio@FMOD@@QAG?AW4FMOD_RESULT@@HIIPAX@Z", 20) {
    OBJ(FMOD_STUDIO_SYSTEM, K_SYSTEM, 0);
    void *extra = GP(4);  // the WAV writer's file name, if the game chose that output
    const char *wav = getenv("NOITAMAC_AUDIO_WAV");  // record the mix instead of playing it, to check audio without ears
    FMOD_SYSTEM *core = NULL;
    if (wav && F->Studio_System_GetCoreSystem(o, &core) == FM_OK && F->System_SetOutput(core, FM_OUTPUTTYPE_WAVWRITER) == FM_OK)
        extra = (void *)wav;
    RET(F->Studio_System_Initialize(o, (int32_t)ARG(1), ARG(2), ARG(3), extra));
}
HOST(fmodstudio, sys_release, "?release@System@Studio@FMOD@@QAG?AW4FMOD_RESULT@@XZ", 4) {
    OBJ(FMOD_STUDIO_SYSTEM, K_SYSTEM, 0);
    FMOD_RESULT r = F->Studio_System_Release(o);
    handle_retire(ARG(0), K_SYSTEM);
    RET(r);
}
HOST(fmodstudio, sys_update, "?update@System@Studio@FMOD@@QAG?AW4FMOD_RESULT@@XZ", 4) { OBJ(FMOD_STUDIO_SYSTEM, K_SYSTEM, 0); RET(F->Studio_System_Update(o)); }
HOST(fmodstudio, sys_isValid, "?isValid@System@Studio@FMOD@@QBG_NXZ", 4) {
    void *o = host_of(ARG(0), K_SYSTEM);
    ret_i32(c, o && F->Studio_System_IsValid(o));
}
HOST(fmodstudio, getCoreSystem, "?getCoreSystem@System@Studio@FMOD@@QBG?AW4FMOD_RESULT@@PAPAV13@@Z", 8) {
    OBJ(FMOD_STUDIO_SYSTEM, K_SYSTEM, 0);
    FMOD_SYSTEM *core = NULL;
    FMOD_RESULT r = F->Studio_System_GetCoreSystem(o, &core);
    out32(ARG(1), wrap(K_CORE, core)), RET(r);
}
HOST(fmodstudio, getBus, "?getBus@System@Studio@FMOD@@QBG?AW4FMOD_RESULT@@PBDPAPAVBus@23@@Z", 12) {
    OBJ(FMOD_STUDIO_SYSTEM, K_SYSTEM, 0);
    FMOD_STUDIO_BUS *b = NULL;
    FMOD_RESULT r = F->Studio_System_GetBus(o, ARG_STR(1), &b);
    out32(ARG(2), wrap(K_BUS, b)), RET(r);
}
HOST(fmodstudio, getEvent, "?getEvent@System@Studio@FMOD@@QBG?AW4FMOD_RESULT@@PBDPAPAVEventDescription@23@@Z", 12) {
    OBJ(FMOD_STUDIO_SYSTEM, K_SYSTEM, 0);
    FMOD_STUDIO_EVENTDESCRIPTION *d = NULL;
    FMOD_RESULT r = F->Studio_System_GetEvent(o, ARG_STR(1), &d);
    out32(ARG(2), wrap(K_DESC, d)), RET(r);
}
HOST(fmodstudio, loadBankFile, "?loadBankFile@System@Studio@FMOD@@QAG?AW4FMOD_RESULT@@PBDIPAPAVBank@23@@Z", 16) {
    OBJ(FMOD_STUDIO_SYSTEM, K_SYSTEM, 0);
    char path[PATH_MAX];
    if (!ARG(1) || !host_path(ARG_STR(1), path, sizeof path)) { ret_i32(c, 18); return; }  // FMOD_ERR_FILE_NOTFOUND
    FMOD_STUDIO_BANK *b = NULL;
    FMOD_RESULT r = F->Studio_System_LoadBankFile(o, path, ARG(2), &b);
    out32(ARG(3), wrap(K_BANK, b)), RET(r);
}
HOST(fmodstudio, sys_setCallback, "?setCallback@System@Studio@FMOD@@QAG?AW4FMOD_RESULT@@P6G?AW44@PAUFMOD_STUDIO_SYSTEM@@IPAX1@ZI@Z", 12) {
    OBJ(FMOD_STUDIO_SYSTEM, K_SYSTEM, 0);
    __atomic_store_n(&SYS_CB, ARG(1), __ATOMIC_RELEASE);
    RET(F->Studio_System_SetCallback(o, ARG(1) ? system_callback : NULL, ARG(2)));
}
HOST(fmodstudio, setListenerAttributes, "?setListenerAttributes@System@Studio@FMOD@@QAG?AW4FMOD_RESULT@@HPBUFMOD_3D_ATTRIBUTES@@PBUFMOD_VECTOR@@@Z", 16) {
    OBJ(FMOD_STUDIO_SYSTEM, K_SYSTEM, 0);
    RET(F->Studio_System_SetListenerAttributes(o, (int32_t)ARG(1), GP(2), GP(3)));
}

// Bank.
HOST(fmodstudio, getEventCount, "?getEventCount@Bank@Studio@FMOD@@QBG?AW4FMOD_RESULT@@PAH@Z", 8) {
    OBJ(FMOD_STUDIO_BANK, K_BANK, 0);
    RET(F->Studio_Bank_GetEventCount(o, GP(1)));
}
HOST(fmodstudio, getEventList, "?getEventList@Bank@Studio@FMOD@@QBG?AW4FMOD_RESULT@@PAPAVEventDescription@23@HPAH@Z", 16) {
    OBJ(FMOD_STUDIO_BANK, K_BANK, 0);
    int cap = (int32_t)ARG(2), n = 0;
    FMOD_STUDIO_EVENTDESCRIPTION **list = calloc(cap > 0 ? cap : 1, sizeof *list);
    FMOD_RESULT r = F->Studio_Bank_GetEventList(o, ARG(1) ? list : NULL, cap, &n);
    for (int i = 0; ARG(1) && i < n && i < cap; i++) wr32(ARG(1) + 4 * i, wrap(K_DESC, list[i]));
    free(list);
    out32(ARG(3), n), RET(r);
}

// EventDescription.
HOST(fmodstudio, createInstance, "?createInstance@EventDescription@Studio@FMOD@@QBG?AW4FMOD_RESULT@@PAPAVEventInstance@23@@Z", 8) {
    OBJ(FMOD_STUDIO_EVENTDESCRIPTION, K_DESC, 0);
    FMOD_STUDIO_EVENTINSTANCE *i = NULL;
    FMOD_RESULT r = F->Studio_EventDescription_CreateInstance(o, &i);
    uint32_t h = 0;
    if (i) {
        h = handle_new(K_INSTANCE, i);
        F->Studio_EventInstance_SetUserData(i, (void *)(uintptr_t)h);
        F->Studio_EventInstance_SetCallback(i, event_callback, FM_EVENT_CALLBACK_DESTROYED);
    }
    out32(ARG(1), h), RET(r);
}
HOST(fmodstudio, getLength, "?getLength@EventDescription@Studio@FMOD@@QBG?AW4FMOD_RESULT@@PAH@Z", 8) {
    OBJ(FMOD_STUDIO_EVENTDESCRIPTION, K_DESC, 0);
    RET(F->Studio_EventDescription_GetLength(o, GP(1)));
}
HOST(fmodstudio, getMinimumDistance, "?getMinimumDistance@EventDescription@Studio@FMOD@@QBG?AW4FMOD_RESULT@@PAM@Z", 8) {
    OBJ(FMOD_STUDIO_EVENTDESCRIPTION, K_DESC, 0);
    RET(F->Studio_EventDescription_GetMinimumDistance(o, GP(1)));
}
HOST(fmodstudio, getMaximumDistance, "?getMaximumDistance@EventDescription@Studio@FMOD@@QBG?AW4FMOD_RESULT@@PAM@Z", 8) {
    OBJ(FMOD_STUDIO_EVENTDESCRIPTION, K_DESC, 0);
    RET(F->Studio_EventDescription_GetMaximumDistance(o, GP(1)));
}
HOST(fmodstudio, getPath, "?getPath@EventDescription@Studio@FMOD@@QBG?AW4FMOD_RESULT@@PADHPAH@Z", 16) {
    OBJ(FMOD_STUDIO_EVENTDESCRIPTION, K_DESC, 0);
    RET(F->Studio_EventDescription_GetPath(o, GP(1), (int32_t)ARG(2), GP(3)));
}
// The guest's FMOD_STUDIO_USER_PROPERTY is {name, type, value} with 32-bit pointers; strings are interned.
HOST(fmodstudio, getUserProperty, "?getUserProperty@EventDescription@Studio@FMOD@@QBG?AW4FMOD_RESULT@@PBDPAUFMOD_STUDIO_USER_PROPERTY@@@Z", 12) {
    OBJ(FMOD_STUDIO_EVENTDESCRIPTION, K_DESC, 0);
    FMOD_STUDIO_USER_PROPERTY p = {0};
    FMOD_RESULT r = F->Studio_EventDescription_GetUserProperty(o, ARG_STR(1), &p);
    if (r == FM_OK && ARG(2)) {
        uint32_t v;
        if (p.type == FM_USER_PROPERTY_STRING) v = intern(p.stringvalue);
        else memcpy(&v, &p.intvalue, 4);  // int, bool (FMOD_BOOL) or float
        wr32(ARG(2), intern(p.name)), wr32(ARG(2) + 4, (uint32_t)p.type), wr32(ARG(2) + 8, v);
    }
    RET(r);
}
HOST(fmodstudio, isSnapshot, "?isSnapshot@EventDescription@Studio@FMOD@@QBG?AW4FMOD_RESULT@@PA_N@Z", 8) {
    OBJ(FMOD_STUDIO_EVENTDESCRIPTION, K_DESC, 0);
    FMOD_BOOL b = 0;
    FMOD_RESULT r = F->Studio_EventDescription_IsSnapshot(o, &b);
    if (ARG(1)) wr8(ARG(1), b != 0);
    RET(r);
}
HOST(fmodstudio, loadSampleData, "?loadSampleData@EventDescription@Studio@FMOD@@QAG?AW4FMOD_RESULT@@XZ", 4) {
    OBJ(FMOD_STUDIO_EVENTDESCRIPTION, K_DESC, 0);
    RET(F->Studio_EventDescription_LoadSampleData(o));
}

// EventInstance.
HOST(fmodstudio, ei_isValid, "?isValid@EventInstance@Studio@FMOD@@QBG_NXZ", 4) {
    void *o = host_of(ARG(0), K_INSTANCE);
    ret_i32(c, o && F->Studio_EventInstance_IsValid(o));
}
HOST(fmodstudio, ei_release, "?release@EventInstance@Studio@FMOD@@QAG?AW4FMOD_RESULT@@XZ", 4) {
    OBJ(FMOD_STUDIO_EVENTINSTANCE, K_INSTANCE, 0);
    RET(F->Studio_EventInstance_Release(o));  // the handle stays valid until the DESTROYED callback
}
HOST(fmodstudio, ei_getUserData, "?getUserData@EventInstance@Studio@FMOD@@QBG?AW4FMOD_RESULT@@PAPAX@Z", 8) {
    Entry *e = entry(ARG(0), K_INSTANCE);
    out32(ARG(1), e ? e->user : 0), ret_i32(c, e ? FM_OK : FM_ERR_INVALID_HANDLE);
}
HOST(fmodstudio, ei_setUserData, "?setUserData@EventInstance@Studio@FMOD@@QAG?AW4FMOD_RESULT@@PAX@Z", 8) {
    Entry *e = entry(ARG(0), K_INSTANCE);
    if (e) e->user = ARG(1);
    ret_i32(c, e ? FM_OK : FM_ERR_INVALID_HANDLE);
}
HOST(fmodstudio, getTimelinePosition, "?getTimelinePosition@EventInstance@Studio@FMOD@@QBG?AW4FMOD_RESULT@@PAH@Z", 8) {
    OBJ(FMOD_STUDIO_EVENTINSTANCE, K_INSTANCE, 0);
    RET(F->Studio_EventInstance_GetTimelinePosition(o, GP(1)));
}
HOST(fmodstudio, set3DAttributes, "?set3DAttributes@EventInstance@Studio@FMOD@@QAG?AW4FMOD_RESULT@@PBUFMOD_3D_ATTRIBUTES@@@Z", 8) {
    OBJ(FMOD_STUDIO_EVENTINSTANCE, K_INSTANCE, 0);
    RET(F->Studio_EventInstance_Set3DAttributes(o, GP(1)));
}
HOST(fmodstudio, ei_setCallback, "?setCallback@EventInstance@Studio@FMOD@@QAG?AW4FMOD_RESULT@@P6G?AW44@IPAUFMOD_STUDIO_EVENTINSTANCE@@PAX@ZI@Z", 12) {
    Entry *e = entry(ARG(0), K_INSTANCE);
    if (!e) { ret_i32(c, FM_ERR_INVALID_HANDLE); return; }
    e->cb = ARG(1), e->mask = ARG(1) ? ARG(2) : 0;
    RET(F->Studio_EventInstance_SetCallback(e->host, event_callback, e->mask | FM_EVENT_CALLBACK_DESTROYED));
}
HOST(fmodstudio, setParameterByName, "?setParameterByName@EventInstance@Studio@FMOD@@QAG?AW4FMOD_RESULT@@PBDM_N@Z", 16) {
    OBJ(FMOD_STUDIO_EVENTINSTANCE, K_INSTANCE, 0);
    RET(F->Studio_EventInstance_SetParameterByName(o, ARG_STR(1), ARG_F32(2), ARG(3) & 0xff));
}
HOST(fmodstudio, ei_setPaused, "?setPaused@EventInstance@Studio@FMOD@@QAG?AW4FMOD_RESULT@@_N@Z", 8) {
    OBJ(FMOD_STUDIO_EVENTINSTANCE, K_INSTANCE, 0);
    RET(F->Studio_EventInstance_SetPaused(o, ARG(1) & 0xff));
}
HOST(fmodstudio, ei_setVolume, "?setVolume@EventInstance@Studio@FMOD@@QAG?AW4FMOD_RESULT@@M@Z", 8) {
    OBJ(FMOD_STUDIO_EVENTINSTANCE, K_INSTANCE, 0);
    RET(F->Studio_EventInstance_SetVolume(o, ARG_F32(1)));
}
HOST(fmodstudio, ei_start, "?start@EventInstance@Studio@FMOD@@QAG?AW4FMOD_RESULT@@XZ", 4) {
    OBJ(FMOD_STUDIO_EVENTINSTANCE, K_INSTANCE, 0);
    RET(F->Studio_EventInstance_Start(o));
}
HOST(fmodstudio, ei_stop, "?stop@EventInstance@Studio@FMOD@@QAG?AW4FMOD_RESULT@@W4FMOD_STUDIO_STOP_MODE@@@Z", 8) {
    OBJ(FMOD_STUDIO_EVENTINSTANCE, K_INSTANCE, 0);
    RET(F->Studio_EventInstance_Stop(o, (FMOD_STUDIO_STOP_MODE)ARG(1)));
}

// Bus.
HOST(fmodstudio, bus_setPaused, "?setPaused@Bus@Studio@FMOD@@QAG?AW4FMOD_RESULT@@_N@Z", 8) {
    OBJ(FMOD_STUDIO_BUS, K_BUS, 0);
    RET(F->Studio_Bus_SetPaused(o, ARG(1) & 0xff));
}
HOST(fmodstudio, bus_setVolume, "?setVolume@Bus@Studio@FMOD@@QAG?AW4FMOD_RESULT@@M@Z", 8) {
    OBJ(FMOD_STUDIO_BUS, K_BUS, 0);
    RET(F->Studio_Bus_SetVolume(o, ARG_F32(1)));
}
