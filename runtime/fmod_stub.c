// FMOD Studio 2.01.05 (fmodstudio.dll, fmod.dll) stubbed natively (HLE): silent audio. The 42 imports are
// C++ member functions with __stdcall, so `this` is the first stack argument. Every call succeeds and no
// callback ever fires; the only data is what the game reads back:
//   - Handles are 16-byte guest-heap objects {magic, kind, user data, name/description}, so isValid and
//     get/setUserData behave. System, core system and master channel group are made once per create;
//     banks, buses and event descriptions once per path (the game keys maps by description handle).
//   - getVersion reports 2.01.05: the game rejects anything older (0x47a738).
//   - Banks hold no events: getEventCount gives 0, so the game skips getEventList (0x47c177).
//   - getUserProperty gives FMOD_ERR_EVENT_NOTFOUND, which the game treats as "no such property"
//     (0x47ba60, 0x47bfb4). Its string variant would dereference the value otherwise.
// This is what NOITAMAC_AUDIO=stub will select once a bridge to the native macOS FMOD exists.
#include <os/lock.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "heap.h"
#include "hle.h"
#include "host.h"

enum { FMOD_OK = 0, FMOD_ERR_EVENT_NOTFOUND = 74, FMOD_VERSION = 0x00020105 };
enum { K_SYSTEM = 1, K_CORE, K_CHANNELGROUP, K_BANK, K_BUS, K_DESC, K_INSTANCE };
#define MAGIC 0x444f4d46u  // "FMOD"
enum { H_MAGIC = 0, H_KIND = 4, H_USER = 8, H_NAME = 12 };  // H_NAME: path, or an instance's description

static os_unfair_lock LOCK = OS_UNFAIR_LOCK_INIT;

static uint32_t handle_new(uint32_t kind, uint32_t name) {
    uint32_t h = heap_calloc(1, 16);
    wr32(h + H_MAGIC, MAGIC), wr32(h + H_KIND, kind), wr32(h + H_NAME, name);
    return h;
}
static bool handle_valid(uint32_t h, uint32_t kind) {
    return h && heap_owns(h) && rd32(h + H_MAGIC) == MAGIC && rd32(h + H_KIND) == kind;
}
static void handle_free(uint32_t h) {
    wr32(h + H_MAGIC, 0);
    heap_free(h);
}

// Named handles (banks, buses, event descriptions), one per (kind, path), kept for the process lifetime.
typedef struct Named { struct Named *next; uint32_t kind, h; char name[]; } Named;
static Named *NAMED[256];
static uint32_t named(uint32_t kind, const char *name) {
    if (!name) name = "";
    uint32_t hash = kind;
    for (const char *s = name; *s; s++) hash = hash * 31 + (uint8_t)*s;
    os_unfair_lock_lock(&LOCK);
    Named **b = &NAMED[hash % 256], *n = *b;
    while (n && (n->kind != kind || strcmp(n->name, name))) n = n->next;
    if (!n) {
        n = malloc(sizeof *n + strlen(name) + 1);
        n->next = *b, n->kind = kind, n->h = handle_new(kind, guest_strdup(name));
        strcpy(n->name, name);
        *b = n;
    }
    os_unfair_lock_unlock(&LOCK);
    return n->h;
}

static void out32(uint32_t p, uint32_t v) { if (p) wr32(p, v); }
static void ok(CPU *c) { ret_i32(c, FMOD_OK); }

// Core (fmod.dll).
HOST(fmod, getMasterChannelGroup, "?getMasterChannelGroup@System@FMOD@@QAG?AW4FMOD_RESULT@@PAPAVChannelGroup@2@@Z", 8) {
    out32(ARG(1), rd32(ARG(0) + H_USER)), ok(c);  // made with the core system, kept in its user data slot
}
HOST(fmod, getVersion, "?getVersion@System@FMOD@@QAG?AW4FMOD_RESULT@@PAI@Z", 8) { out32(ARG(1), FMOD_VERSION), ok(c); }
HOST(fmod, set3DSettings, "?set3DSettings@System@FMOD@@QAG?AW4FMOD_RESULT@@MMM@Z", 16) { ok(c); }
HOST(fmod, setDSPBufferSize, "?setDSPBufferSize@System@FMOD@@QAG?AW4FMOD_RESULT@@IH@Z", 12) { ok(c); }
HOST(fmod, setOutput, "?setOutput@System@FMOD@@QAG?AW4FMOD_RESULT@@W4FMOD_OUTPUTTYPE@@@Z", 8) { ok(c); }
HOST(fmod, setSoftwareChannels, "?setSoftwareChannels@System@FMOD@@QAG?AW4FMOD_RESULT@@H@Z", 8) { ok(c); }
HOST(fmod, cc_setVolume, "?setVolume@ChannelControl@FMOD@@QAG?AW4FMOD_RESULT@@M@Z", 8) { ok(c); }

// Studio::System. Its H_NAME slot holds the core system.
HOST(fmodstudio, create, "?create@System@Studio@FMOD@@SG?AW4FMOD_RESULT@@PAPAV123@I@Z", 8) {
    const char *audio = getenv("NOITAMAC_AUDIO");
    if (audio && strcmp(audio, "stub")) fprintf(stderr, "[noitamac] NOITAMAC_AUDIO=%s: only the silent stub exists\n", audio);
    uint32_t core = handle_new(K_CORE, 0);
    wr32(core + H_USER, handle_new(K_CHANNELGROUP, 0));
    out32(ARG(0), handle_new(K_SYSTEM, core)), ok(c);
}
HOST(fmodstudio, sys_initialize, "?initialize@System@Studio@FMOD@@QAG?AW4FMOD_RESULT@@HIIPAX@Z", 20) { ok(c); }
HOST(fmodstudio, sys_release, "?release@System@Studio@FMOD@@QAG?AW4FMOD_RESULT@@XZ", 4) {
    uint32_t s = ARG(0);
    if (handle_valid(s, K_SYSTEM)) {
        uint32_t core = rd32(s + H_NAME);
        handle_free(rd32(core + H_USER)), handle_free(core), handle_free(s);
    }
    ok(c);
}
HOST(fmodstudio, sys_update, "?update@System@Studio@FMOD@@QAG?AW4FMOD_RESULT@@XZ", 4) { ok(c); }
HOST(fmodstudio, sys_isValid, "?isValid@System@Studio@FMOD@@QBG_NXZ", 4) { ret_i32(c, handle_valid(ARG(0), K_SYSTEM)); }
HOST(fmodstudio, getCoreSystem, "?getCoreSystem@System@Studio@FMOD@@QBG?AW4FMOD_RESULT@@PAPAV13@@Z", 8) {
    out32(ARG(1), rd32(ARG(0) + H_NAME)), ok(c);
}
HOST(fmodstudio, getBus, "?getBus@System@Studio@FMOD@@QBG?AW4FMOD_RESULT@@PBDPAPAVBus@23@@Z", 12) {
    out32(ARG(2), named(K_BUS, ARG_STR(1))), ok(c);
}
HOST(fmodstudio, getEvent, "?getEvent@System@Studio@FMOD@@QBG?AW4FMOD_RESULT@@PBDPAPAVEventDescription@23@@Z", 12) {
    out32(ARG(2), named(K_DESC, ARG_STR(1))), ok(c);
}
HOST(fmodstudio, loadBankFile, "?loadBankFile@System@Studio@FMOD@@QAG?AW4FMOD_RESULT@@PBDIPAPAVBank@23@@Z", 16) {
    out32(ARG(3), named(K_BANK, ARG_STR(1))), ok(c);
}
HOST(fmodstudio, sys_setCallback, "?setCallback@System@Studio@FMOD@@QAG?AW4FMOD_RESULT@@P6G?AW44@PAUFMOD_STUDIO_SYSTEM@@IPAX1@ZI@Z", 12) { ok(c); }
HOST(fmodstudio, setListenerAttributes, "?setListenerAttributes@System@Studio@FMOD@@QAG?AW4FMOD_RESULT@@HPBUFMOD_3D_ATTRIBUTES@@PBUFMOD_VECTOR@@@Z", 16) { ok(c); }

// Bank.
HOST(fmodstudio, getEventCount, "?getEventCount@Bank@Studio@FMOD@@QBG?AW4FMOD_RESULT@@PAH@Z", 8) { out32(ARG(1), 0), ok(c); }
HOST(fmodstudio, getEventList, "?getEventList@Bank@Studio@FMOD@@QBG?AW4FMOD_RESULT@@PAPAVEventDescription@23@HPAH@Z", 16) {
    out32(ARG(3), 0), ok(c);
}

// EventDescription. Its H_NAME slot holds its path.
HOST(fmodstudio, createInstance, "?createInstance@EventDescription@Studio@FMOD@@QBG?AW4FMOD_RESULT@@PAPAVEventInstance@23@@Z", 8) {
    out32(ARG(1), handle_new(K_INSTANCE, ARG(0))), ok(c);
}
HOST(fmodstudio, getLength, "?getLength@EventDescription@Studio@FMOD@@QBG?AW4FMOD_RESULT@@PAH@Z", 8) { out32(ARG(1), 0), ok(c); }
HOST(fmodstudio, getMinimumDistance, "?getMinimumDistance@EventDescription@Studio@FMOD@@QBG?AW4FMOD_RESULT@@PAM@Z", 8) {
    if (ARG(1)) wrf32(ARG(1), 1.0f);
    ok(c);
}
HOST(fmodstudio, getMaximumDistance, "?getMaximumDistance@EventDescription@Studio@FMOD@@QBG?AW4FMOD_RESULT@@PAM@Z", 8) {
    if (ARG(1)) wrf32(ARG(1), 20.0f);
    ok(c);
}
HOST(fmodstudio, getPath, "?getPath@EventDescription@Studio@FMOD@@QBG?AW4FMOD_RESULT@@PADHPAH@Z", 16) {
    uint32_t d = ARG(0), buf = ARG(1), size = ARG(2);
    const char *path = handle_valid(d, K_DESC) ? (const char *)P(rd32(d + H_NAME)) : "";
    uint32_t n = strlen(path) + 1;
    if (buf && (int32_t)size > 0) {
        uint32_t k = n < size ? n : size;
        memcpy(P(buf), path, k - 1);
        wr8(buf + k - 1, 0);
    }
    out32(ARG(3), n), ok(c);
}
HOST(fmodstudio, getUserProperty, "?getUserProperty@EventDescription@Studio@FMOD@@QBG?AW4FMOD_RESULT@@PBDPAUFMOD_STUDIO_USER_PROPERTY@@@Z", 12) {
    ret_i32(c, FMOD_ERR_EVENT_NOTFOUND);
}
HOST(fmodstudio, isSnapshot, "?isSnapshot@EventDescription@Studio@FMOD@@QBG?AW4FMOD_RESULT@@PA_N@Z", 8) {
    if (ARG(1)) wr8(ARG(1), 0);
    ok(c);
}
HOST(fmodstudio, loadSampleData, "?loadSampleData@EventDescription@Studio@FMOD@@QAG?AW4FMOD_RESULT@@XZ", 4) { ok(c); }

// EventInstance.
HOST(fmodstudio, ei_isValid, "?isValid@EventInstance@Studio@FMOD@@QBG_NXZ", 4) { ret_i32(c, handle_valid(ARG(0), K_INSTANCE)); }
HOST(fmodstudio, ei_release, "?release@EventInstance@Studio@FMOD@@QAG?AW4FMOD_RESULT@@XZ", 4) {
    if (handle_valid(ARG(0), K_INSTANCE)) handle_free(ARG(0));
    ok(c);
}
HOST(fmodstudio, ei_getUserData, "?getUserData@EventInstance@Studio@FMOD@@QBG?AW4FMOD_RESULT@@PAPAX@Z", 8) {
    out32(ARG(1), handle_valid(ARG(0), K_INSTANCE) ? rd32(ARG(0) + H_USER) : 0), ok(c);
}
HOST(fmodstudio, ei_setUserData, "?setUserData@EventInstance@Studio@FMOD@@QAG?AW4FMOD_RESULT@@PAX@Z", 8) {
    if (handle_valid(ARG(0), K_INSTANCE)) wr32(ARG(0) + H_USER, ARG(1));
    ok(c);
}
HOST(fmodstudio, getTimelinePosition, "?getTimelinePosition@EventInstance@Studio@FMOD@@QBG?AW4FMOD_RESULT@@PAH@Z", 8) {
    out32(ARG(1), 0), ok(c);
}
HOST(fmodstudio, set3DAttributes, "?set3DAttributes@EventInstance@Studio@FMOD@@QAG?AW4FMOD_RESULT@@PBUFMOD_3D_ATTRIBUTES@@@Z", 8) { ok(c); }
HOST(fmodstudio, ei_setCallback, "?setCallback@EventInstance@Studio@FMOD@@QAG?AW4FMOD_RESULT@@P6G?AW44@IPAUFMOD_STUDIO_EVENTINSTANCE@@PAX@ZI@Z", 12) { ok(c); }
HOST(fmodstudio, setParameterByName, "?setParameterByName@EventInstance@Studio@FMOD@@QAG?AW4FMOD_RESULT@@PBDM_N@Z", 16) { ok(c); }
HOST(fmodstudio, ei_setPaused, "?setPaused@EventInstance@Studio@FMOD@@QAG?AW4FMOD_RESULT@@_N@Z", 8) { ok(c); }
HOST(fmodstudio, ei_setVolume, "?setVolume@EventInstance@Studio@FMOD@@QAG?AW4FMOD_RESULT@@M@Z", 8) { ok(c); }
HOST(fmodstudio, ei_start, "?start@EventInstance@Studio@FMOD@@QAG?AW4FMOD_RESULT@@XZ", 4) { ok(c); }
HOST(fmodstudio, ei_stop, "?stop@EventInstance@Studio@FMOD@@QAG?AW4FMOD_RESULT@@W4FMOD_STUDIO_STOP_MODE@@@Z", 8) { ok(c); }

// Bus.
HOST(fmodstudio, bus_setPaused, "?setPaused@Bus@Studio@FMOD@@QAG?AW4FMOD_RESULT@@_N@Z", 8) { ok(c); }
HOST(fmodstudio, bus_setVolume, "?setVolume@Bus@Studio@FMOD@@QAG?AW4FMOD_RESULT@@M@Z", 8) { ok(c); }
