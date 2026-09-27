// The silent FMOD backend (MINA4MAC_AUDIO=stub, or when the macOS FMOD dylibs aren't installed): a stand-in
// for the part of the FMOD C API the bridge (fmod.c) calls. Every call succeeds and nothing plays; the only
// data is what the game reads back:
//   - Objects are host allocations. The system, core system and master channel group are made once per
//     create; banks, buses and event descriptions once per path (the game keys maps by description handle).
//   - getVersion reports 2.01.05: the game rejects anything older (0x47a738).
//   - Banks hold no events: getEventCount gives 0, so the game skips getEventList (0x47c177).
//   - getUserProperty gives FMOD_ERR_EVENT_NOTFOUND, which the game treats as "no such property"
//     (0x47ba60, 0x47bfb4).
//   - An instance never starts or stops, so the game never releases it from its STOPPED callback. Releasing
//     one destroys it at once, with the DESTROYED callback the bridge needs to retire its handle.
#include <os/lock.h>
#include <stdlib.h>
#include <string.h>

#include "fmod.h"

enum { FMOD_VERSION_STUB = 0x00020105 };
enum { K_SYSTEM = 1, K_CORE, K_CHANNELGROUP, K_BANK, K_BUS, K_DESC, K_INSTANCE };

typedef struct Obj {
    struct Obj *next;  // named objects: hash chain; the system: its core system
    int kind;
    void *user;
    FMOD_STUDIO_EVENT_CALLBACK cb;
    FMOD_STUDIO_EVENT_CALLBACK_TYPE mask;
    struct Obj *aux;   // the core system's master channel group; an instance's description
    char name[];
} Obj;

static os_unfair_lock LOCK = OS_UNFAIR_LOCK_INIT;
static Obj *NAMED[256];

static Obj *obj_new(int kind, const char *name) {
    Obj *o = calloc(1, sizeof *o + strlen(name) + 1);
    o->kind = kind;
    strcpy(o->name, name);
    return o;
}

// Named objects (banks, buses, event descriptions), one per (kind, path), kept for the process lifetime.
static Obj *named(int kind, const char *name) {
    if (!name) name = "";
    uint32_t hash = kind;
    for (const char *s = name; *s; s++) hash = hash * 31 + (uint8_t)*s;
    os_unfair_lock_lock(&LOCK);
    Obj **b = &NAMED[hash % 256], *n = *b;
    while (n && (n->kind != kind || strcmp(n->name, name))) n = n->next;
    if (!n) n = obj_new(kind, name), n->next = *b, *b = n;
    os_unfair_lock_unlock(&LOCK);
    return n;
}

#define O(p) ((Obj *)(p))

static FMOD_RESULT thread_setattributes(FMOD_THREAD_TYPE t, FMOD_THREAD_AFFINITY a, FMOD_THREAD_PRIORITY p, FMOD_THREAD_STACK_SIZE s) {
    (void)t, (void)a, (void)p, (void)s;
    return FM_OK;
}
static FMOD_RESULT sys_getmastercg(FMOD_SYSTEM *s, FMOD_CHANNELGROUP **cg) { *cg = (FMOD_CHANNELGROUP *)O(s)->aux; return FM_OK; }
static FMOD_RESULT sys_getversion(FMOD_SYSTEM *s, unsigned int *v) { (void)s, *v = FMOD_VERSION_STUB; return FM_OK; }
static FMOD_RESULT sys_set3d(FMOD_SYSTEM *s, float d, float f, float r) { (void)s, (void)d, (void)f, (void)r; return FM_OK; }
static FMOD_RESULT sys_setdspbuffer(FMOD_SYSTEM *s, unsigned int n, int k) { (void)s, (void)n, (void)k; return FM_OK; }
static FMOD_RESULT sys_setoutput(FMOD_SYSTEM *s, FMOD_OUTPUTTYPE t) { (void)s, (void)t; return FM_OK; }
static FMOD_RESULT sys_setsoftwarechannels(FMOD_SYSTEM *s, int n) { (void)s, (void)n; return FM_OK; }
static FMOD_RESULT cg_setvolume(FMOD_CHANNELGROUP *g, float v) { (void)g, (void)v; return FM_OK; }

static FMOD_RESULT studio_create(FMOD_STUDIO_SYSTEM **out, unsigned int version) {
    (void)version;
    Obj *s = obj_new(K_SYSTEM, ""), *core = obj_new(K_CORE, "");
    core->aux = obj_new(K_CHANNELGROUP, "");
    s->next = core;
    *out = (FMOD_STUDIO_SYSTEM *)s;
    return FM_OK;
}
static FMOD_RESULT studio_initialize(FMOD_STUDIO_SYSTEM *s, int n, FMOD_STUDIO_INITFLAGS sf, FMOD_INITFLAGS f, void *x) {
    (void)s, (void)n, (void)sf, (void)f, (void)x;
    return FM_OK;
}
static FMOD_RESULT studio_release(FMOD_STUDIO_SYSTEM *s) {
    free(O(s)->next->aux), free(O(s)->next), free(s);
    return FM_OK;
}
static FMOD_RESULT studio_update(FMOD_STUDIO_SYSTEM *s) { (void)s; return FM_OK; }
static FMOD_BOOL studio_isvalid(FMOD_STUDIO_SYSTEM *s) { return s != NULL; }
static FMOD_RESULT studio_getcore(FMOD_STUDIO_SYSTEM *s, FMOD_SYSTEM **core) { *core = (FMOD_SYSTEM *)O(s)->next; return FM_OK; }
static FMOD_RESULT studio_getbus(FMOD_STUDIO_SYSTEM *s, const char *p, FMOD_STUDIO_BUS **b) {
    (void)s, *b = (FMOD_STUDIO_BUS *)named(K_BUS, p);
    return FM_OK;
}
static FMOD_RESULT studio_getevent(FMOD_STUDIO_SYSTEM *s, const char *p, FMOD_STUDIO_EVENTDESCRIPTION **d) {
    (void)s, *d = (FMOD_STUDIO_EVENTDESCRIPTION *)named(K_DESC, p);
    return FM_OK;
}
static FMOD_RESULT studio_loadbank(FMOD_STUDIO_SYSTEM *s, const char *p, FMOD_STUDIO_LOAD_BANK_FLAGS f, FMOD_STUDIO_BANK **b) {
    (void)s, (void)f, *b = (FMOD_STUDIO_BANK *)named(K_BANK, p);
    return FM_OK;
}
static FMOD_RESULT studio_setcallback(FMOD_STUDIO_SYSTEM *s, FMOD_STUDIO_SYSTEM_CALLBACK cb, FMOD_STUDIO_SYSTEM_CALLBACK_TYPE m) {
    (void)s, (void)cb, (void)m;
    return FM_OK;
}
static FMOD_RESULT studio_setlistener(FMOD_STUDIO_SYSTEM *s, int i, const FMOD_3D_ATTRIBUTES *a, const FMOD_VECTOR *v) {
    (void)s, (void)i, (void)a, (void)v;
    return FM_OK;
}

static FMOD_RESULT bank_geteventcount(FMOD_STUDIO_BANK *b, int *n) { (void)b, *n = 0; return FM_OK; }
static FMOD_RESULT bank_geteventlist(FMOD_STUDIO_BANK *b, FMOD_STUDIO_EVENTDESCRIPTION **a, int cap, int *n) {
    (void)b, (void)a, (void)cap;
    if (n) *n = 0;
    return FM_OK;
}

static FMOD_RESULT desc_createinstance(FMOD_STUDIO_EVENTDESCRIPTION *d, FMOD_STUDIO_EVENTINSTANCE **out) {
    Obj *i = obj_new(K_INSTANCE, "");
    i->aux = O(d);
    *out = (FMOD_STUDIO_EVENTINSTANCE *)i;
    return FM_OK;
}
static FMOD_RESULT desc_getlength(FMOD_STUDIO_EVENTDESCRIPTION *d, int *n) { (void)d, *n = 0; return FM_OK; }
static FMOD_RESULT desc_getmindistance(FMOD_STUDIO_EVENTDESCRIPTION *d, float *v) { (void)d, *v = 1.0f; return FM_OK; }
static FMOD_RESULT desc_getmaxdistance(FMOD_STUDIO_EVENTDESCRIPTION *d, float *v) { (void)d, *v = 20.0f; return FM_OK; }
static FMOD_RESULT desc_getpath(FMOD_STUDIO_EVENTDESCRIPTION *d, char *buf, int size, int *retrieved) {
    const char *path = O(d)->name;
    int n = (int)strlen(path) + 1;
    if (buf && size > 0) {
        int k = n < size ? n : size;
        memcpy(buf, path, k - 1);
        buf[k - 1] = 0;
    }
    if (retrieved) *retrieved = n;
    return FM_OK;
}
static FMOD_RESULT desc_getuserproperty(FMOD_STUDIO_EVENTDESCRIPTION *d, const char *name, FMOD_STUDIO_USER_PROPERTY *p) {
    (void)d, (void)name, (void)p;
    return FM_ERR_EVENT_NOTFOUND;
}
static FMOD_RESULT desc_issnapshot(FMOD_STUDIO_EVENTDESCRIPTION *d, FMOD_BOOL *v) { (void)d, *v = 0; return FM_OK; }
static FMOD_RESULT desc_loadsampledata(FMOD_STUDIO_EVENTDESCRIPTION *d) { (void)d; return FM_OK; }

static FMOD_BOOL ei_isvalid(FMOD_STUDIO_EVENTINSTANCE *i) { return i != NULL; }
static FMOD_RESULT ei_release(FMOD_STUDIO_EVENTINSTANCE *i) {
    if (O(i)->cb && O(i)->mask & FM_EVENT_CALLBACK_DESTROYED) O(i)->cb(FM_EVENT_CALLBACK_DESTROYED, i, NULL);
    free(i);
    return FM_OK;
}
static FMOD_RESULT ei_getuserdata(FMOD_STUDIO_EVENTINSTANCE *i, void **u) { *u = O(i)->user; return FM_OK; }
static FMOD_RESULT ei_setuserdata(FMOD_STUDIO_EVENTINSTANCE *i, void *u) { O(i)->user = u; return FM_OK; }
static FMOD_RESULT ei_gettimelineposition(FMOD_STUDIO_EVENTINSTANCE *i, int *p) { (void)i, *p = 0; return FM_OK; }
static FMOD_RESULT ei_set3dattributes(FMOD_STUDIO_EVENTINSTANCE *i, FMOD_3D_ATTRIBUTES *a) { (void)i, (void)a; return FM_OK; }
static FMOD_RESULT ei_setcallback(FMOD_STUDIO_EVENTINSTANCE *i, FMOD_STUDIO_EVENT_CALLBACK cb, FMOD_STUDIO_EVENT_CALLBACK_TYPE m) {
    O(i)->cb = cb, O(i)->mask = m;
    return FM_OK;
}
static FMOD_RESULT ei_setparameterbyname(FMOD_STUDIO_EVENTINSTANCE *i, const char *n, float v, FMOD_BOOL s) {
    (void)i, (void)n, (void)v, (void)s;
    return FM_OK;
}
static FMOD_RESULT ei_setpaused(FMOD_STUDIO_EVENTINSTANCE *i, FMOD_BOOL p) { (void)i, (void)p; return FM_OK; }
static FMOD_RESULT ei_setvolume(FMOD_STUDIO_EVENTINSTANCE *i, float v) { (void)i, (void)v; return FM_OK; }
static FMOD_RESULT ei_start(FMOD_STUDIO_EVENTINSTANCE *i) { (void)i; return FM_OK; }
static FMOD_RESULT ei_stop(FMOD_STUDIO_EVENTINSTANCE *i, FMOD_STUDIO_STOP_MODE m) { (void)i, (void)m; return FM_OK; }

static FMOD_RESULT bus_setpaused(FMOD_STUDIO_BUS *b, FMOD_BOOL p) { (void)b, (void)p; return FM_OK; }
static FMOD_RESULT bus_setvolume(FMOD_STUDIO_BUS *b, float v) { (void)b, (void)v; return FM_OK; }

const FmodApi FMOD_STUB = {
    .Thread_SetAttributes = thread_setattributes,
    .System_GetMasterChannelGroup = sys_getmastercg,
    .System_GetVersion = sys_getversion,
    .System_Set3DSettings = sys_set3d,
    .System_SetDSPBufferSize = sys_setdspbuffer,
    .System_SetOutput = sys_setoutput,
    .System_SetSoftwareChannels = sys_setsoftwarechannels,
    .ChannelGroup_SetVolume = cg_setvolume,
    .Studio_System_Create = studio_create,
    .Studio_System_Initialize = studio_initialize,
    .Studio_System_Release = studio_release,
    .Studio_System_Update = studio_update,
    .Studio_System_IsValid = studio_isvalid,
    .Studio_System_GetCoreSystem = studio_getcore,
    .Studio_System_GetBus = studio_getbus,
    .Studio_System_GetEvent = studio_getevent,
    .Studio_System_LoadBankFile = studio_loadbank,
    .Studio_System_SetCallback = studio_setcallback,
    .Studio_System_SetListenerAttributes = studio_setlistener,
    .Studio_Bank_GetEventCount = bank_geteventcount,
    .Studio_Bank_GetEventList = bank_geteventlist,
    .Studio_EventDescription_CreateInstance = desc_createinstance,
    .Studio_EventDescription_GetLength = desc_getlength,
    .Studio_EventDescription_GetMinimumDistance = desc_getmindistance,
    .Studio_EventDescription_GetMaximumDistance = desc_getmaxdistance,
    .Studio_EventDescription_GetPath = desc_getpath,
    .Studio_EventDescription_GetUserProperty = desc_getuserproperty,
    .Studio_EventDescription_IsSnapshot = desc_issnapshot,
    .Studio_EventDescription_LoadSampleData = desc_loadsampledata,
    .Studio_EventInstance_IsValid = ei_isvalid,
    .Studio_EventInstance_Release = ei_release,
    .Studio_EventInstance_GetUserData = ei_getuserdata,
    .Studio_EventInstance_SetUserData = ei_setuserdata,
    .Studio_EventInstance_GetTimelinePosition = ei_gettimelineposition,
    .Studio_EventInstance_Set3DAttributes = ei_set3dattributes,
    .Studio_EventInstance_SetCallback = ei_setcallback,
    .Studio_EventInstance_SetParameterByName = ei_setparameterbyname,
    .Studio_EventInstance_SetPaused = ei_setpaused,
    .Studio_EventInstance_SetVolume = ei_setvolume,
    .Studio_EventInstance_Start = ei_start,
    .Studio_EventInstance_Stop = ei_stop,
    .Studio_Bus_SetPaused = bus_setpaused,
    .Studio_Bus_SetVolume = bus_setvolume,
};
