// FMOD bridge test against the native macOS FMOD (built and run by tools/check.sh as fmodtest when
// tools/setup_fmod.sh has installed it in build/fmod_api). fmod.c is compiled with FMOD_REAL_HEADERS, so the
// build also checks the bridge's API table against the SDK's prototypes. Drives the imports through their
// thunks the way the game does (its init sequence, with no-sound output), loads every bank of the game in
// build/game/data/audio/Desktop, and plays events with "recompiled" guest callbacks below, which run on
// FMOD's Studio thread with their own guest thread, get marker/beat parameters in guest memory, and release
// their instance from STOPPED as the game's callback 0x47d0e0 does.
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "heap.h"
#include "host.h"
#include "proc.h"

static int fails, checks;
#define CHECK(what, got, want)                                                                              \
    do {                                                                                                    \
        long long g_ = (long long)(got), w_ = (long long)(want);                                            \
        checks++;                                                                                           \
        if (g_ == w_) printf("  ok %s\n", what);                                                            \
        else printf("FAIL %s: got %lld (%#llx), want %lld\n", what, g_, (unsigned long long)g_, w_), fails++; \
    } while (0)

enum { RET_ADDR = 0x0badc0de };
enum { STOPPED = 0x20, TIMELINE_MARKER = 0x800, TIMELINE_BEAT = 0x1000, DESTROYED = 0x2, POSTUPDATE = 0x2 };

#define STU(name) "?" name "@Studio@FMOD@@"
static uint32_t fs_call(CPU *c, const char *dll, const char *name, int n, const uint32_t *args) {
    uint32_t top = c->esp;
    for (int i = n - 1; i >= 0; i--) { c->esp -= 4; wr32(c->esp, args[i]); }
    c->esp -= 4;
    wr32(c->esp, RET_ADDR);
    guest_call(c, rt_thunk(dll, name));
    uint32_t popped = c->esp - top;
    c->esp = top;
    if (popped != 0) printf("FAIL %s popped %d argument bytes too few\n", name, -(int)popped), fails++;
    return c->eax;
}
#define FS(c, name, ...) fs_call(c, "fmodstudio.dll", name, sizeof((uint32_t[]){__VA_ARGS__}) / 4, (uint32_t[]){__VA_ARGS__})
#define FC(c, name, ...) fs_call(c, "fmod.dll", name, sizeof((uint32_t[]){__VA_ARGS__}) / 4, (uint32_t[]){__VA_ARGS__})
#define EI_GETUSERDATA STU("getUserData@EventInstance") "QBG?AW4FMOD_RESULT@@PAPAX@Z"
#define EI_RELEASE STU("release@EventInstance") "QAG?AW4FMOD_RESULT@@XZ"
#define EI_ISVALID STU("isValid@EventInstance") "QBG_NXZ"

static uint32_t gstr(const char *s) { uint32_t g = heap_alloc(strlen(s) + 1); strcpy((char *)P(g), s); return g; }

// Guest callbacks (__stdcall): what they saw, written on FMOD's thread.
static uint32_t MAIN_TID;
static volatile int SYS_CALLS, SYS_OTHER_THREAD, EV_CALLS, EV_OTHER_THREAD, BEATS, BEAT_OK, MARKERS, STOPS, STOP_USER_OK, RELEASED;
static char MARKER[64];
static uint32_t EXPECT_USER;
enum { G_SYSCB = 0x300000, G_EVCB = 0x300010 };
// FMOD_RESULT system_callback(system, type, commanddata, userdata)
static void F_syscb(CPU *c) {
    SYS_CALLS++;
    SYS_OTHER_THREAD += rd32(c->fs_base + TEB_TID) != MAIN_TID;
    c->eax = 0;
    c->esp += 4 + 16;
}
// FMOD_RESULT event_callback(type, event, parameters), like the game's 0x47d0e0.
static void F_evcb(CPU *c) {
    uint32_t type = rd32(c->esp + 4), ev = rd32(c->esp + 8), p = rd32(c->esp + 12);
    EV_CALLS++;
    EV_OTHER_THREAD += rd32(c->fs_base + TEB_TID) != MAIN_TID;
    if (type == TIMELINE_BEAT && p) {
        BEATS++;  // {bar, beat, position, tempo, upper, lower}
        BEAT_OK += p >= STACKS_LO && rd32(p) >= 1 && rd32(p + 4) >= 1 && rdf32(p + 12) > 0 && rd32(p + 16) >= 1;
    }
    if (type == TIMELINE_MARKER && p) {
        MARKERS++;
        snprintf(MARKER, sizeof MARKER, "%s", (char *)P(rd32(p)));
    }
    if (type == STOPPED) {
        uint32_t out = c->esp - 64;
        c->esp -= 128;
        FS(c, EI_GETUSERDATA, ev, out);
        STOPS++;
        STOP_USER_OK += rd32(out) == EXPECT_USER;
        RELEASED += FS(c, EI_RELEASE, ev) == 0;
        c->esp += 128;
    }
    c->eax = 0;
    c->esp += 4 + 12;
}
const FnEntry FN_TABLE[] = {{G_SYSCB, F_syscb}, {G_EVCB, F_evcb}};
const int FN_COUNT = 2;

static void update(CPU *c, uint32_t sys, int ms) {
    for (int t = 0; t < ms; t += 10) {
        FS(c, STU("update@System") "QAG?AW4FMOD_RESULT@@XZ", sys);
        usleep(10000);
    }
}

int main(void) {
    rt_init();
    CPU cpu = {.fpu_cw = 0x27f}, *c = &cpu;
    rt_thread_init(c);
    MAIN_TID = rd32(c->fs_base + TEB_TID);
    setenv("NOITAMAC_AUDIO", "fmod", 1);  // exit rather than fall back to the stub
    uint32_t out = heap_calloc(16, 4), buf = heap_alloc(256);

    // The game's init sequence (0x47a6f5..0x47a8b7), with no-sound output.
    CHECK("create(2.01.05 header)", FS(c, STU("create@System") "SG?AW4FMOD_RESULT@@PAPAV123@I@Z", out, 0x20105), 0);
    uint32_t sys = rd32(out);
    CHECK("System isValid", FS(c, STU("isValid@System") "QBG_NXZ", sys) & 0xff, 1);
    FS(c, STU("getCoreSystem@System") "QBG?AW4FMOD_RESULT@@PAPAV13@@Z", sys, out);
    uint32_t core = rd32(out);
    FC(c, "?getVersion@System@FMOD@@QAG?AW4FMOD_RESULT@@PAI@Z", core, out);
    CHECK("getVersion is 2.01.x, at least 2.01.05", (rd32(out) & 0xffff00) == 0x20100 && rd32(out) >= 0x20105, 1);
    printf("  FMOD runtime %x\n", rd32(out));
    CHECK("setDSPBufferSize", FC(c, "?setDSPBufferSize@System@FMOD@@QAG?AW4FMOD_RESULT@@IH@Z", core, 0x100, 4), 0);
    CHECK("setSoftwareChannels", FC(c, "?setSoftwareChannels@System@FMOD@@QAG?AW4FMOD_RESULT@@H@Z", core, 0x80), 0);
    CHECK("setOutput(NOSOUND)", FC(c, "?setOutput@System@FMOD@@QAG?AW4FMOD_RESULT@@W4FMOD_OUTPUTTYPE@@@Z", core, 2), 0);
    CHECK("initialize", FS(c, STU("initialize@System") "QAG?AW4FMOD_RESULT@@HIIPAX@Z", sys, 0x200, 0, 0x20200, 0), 0);
    CHECK("setCallback(system, all)",
          FS(c, STU("setCallback@System") "QAG?AW4FMOD_RESULT@@P6G?AW44@PAUFMOD_STUDIO_SYSTEM@@IPAX1@ZI@Z", sys, G_SYSCB, 0xffffffff), 0);
    CHECK("set3DSettings", FC(c, "?set3DSettings@System@FMOD@@QAG?AW4FMOD_RESULT@@MMM@Z", core, 0x42000000, 0x41800000, 0x3f800000), 0);
    FC(c, "?getMasterChannelGroup@System@FMOD@@QAG?AW4FMOD_RESULT@@PAPAVChannelGroup@2@@Z", core, out);
    CHECK("master channel group setVolume", FC(c, "?setVolume@ChannelControl@FMOD@@QAG?AW4FMOD_RESULT@@M@Z", rd32(out), 0x3f000000), 0);

    // Every bank of the game, and every event in them (as 0x47c177 lists them).
    DIR *d = opendir("build/game/data/audio/Desktop");
    CHECK("game banks present (tools/setup_game.sh)", d != NULL, 1);
    if (!d) return 1;
    int banks = 0, banks_ok = 0, events = 0, paths_ok = 0, snapshots = 0;
    uint32_t list = heap_calloc(1024, 4), bank[64];
    for (struct dirent *e; (e = readdir(d));) {
        if (!strstr(e->d_name, ".bank")) continue;
        char path[512];  // as the game passes them: Windows paths on Z:
        snprintf(path, sizeof path, "Z:%s/build/game/data/audio/Desktop/%s", getcwd((char[400]){0}, 400), e->d_name);
        for (char *s = path; *s; s++) if (*s == '/') *s = '\\';
        if (banks++ == 64) break;
        if (FS(c, STU("loadBankFile@System") "QAG?AW4FMOD_RESULT@@PBDIPAPAVBank@23@@Z", sys, gstr(path), 0, out)) continue;
        bank[banks_ok++] = rd32(out);
    }
    closedir(d);
    for (int b = 0; b < banks_ok; b++) {  // paths need the strings bank, so list once all are loaded
        FS(c, STU("getEventCount@Bank") "QBG?AW4FMOD_RESULT@@PAH@Z", bank[b], out);
        int n = rd32(out);
        FS(c, STU("getEventList@Bank") "QBG?AW4FMOD_RESULT@@PAPAVEventDescription@23@HPAH@Z", bank[b], list, 1024, out);
        n = n < (int)rd32(out) ? n : (int)rd32(out);
        for (int i = 0; i < n; i++, events++) {
            uint32_t desc = rd32(list + 4 * i);
            FS(c, STU("getPath@EventDescription") "QBG?AW4FMOD_RESULT@@PADHPAH@Z", desc, buf, 256, out);
            const char *path = (char *)P(buf);
            paths_ok += (!strncmp(path, "event:/", 7) || !strncmp(path, "snapshot:/", 10)) && rd32(out) == strlen(path) + 1;
            FS(c, STU("isSnapshot@EventDescription") "QBG?AW4FMOD_RESULT@@PA_N@Z", desc, out);
            snapshots += rd8(out);
        }
    }
    printf("  %d banks, %d events, %d snapshots\n", banks, events, snapshots);
    CHECK("all banks load", banks_ok == banks && banks > 0, 1);
    CHECK("events listed, paths event:/... or snapshot:/...", events > 0 && paths_ok == events, 1);

    // Descriptions: one handle per event (from getEvent and from the bank lists alike), properties.
#define GETEVENT(p) (FS(c, STU("getEvent@System") "QBG?AW4FMOD_RESULT@@PBDPAPAVEventDescription@23@@Z", sys, gstr(p), out), rd32(out))
    uint32_t click = GETEVENT("event:/ui/button_click");
    CHECK("getEvent twice, same handle", click && GETEVENT("event:/ui/button_click") == click, 1);
    CHECK("getEvent missing", FS(c, STU("getEvent@System") "QBG?AW4FMOD_RESULT@@PBDPAPAVEventDescription@23@@Z", sys, gstr("event:/nope"), out),
          74);
    wr32(buf, 0xdead), wr32(buf + 4, 0xdead);
    CHECK("getUserProperty missing",
          FS(c, STU("getUserProperty@EventDescription") "QBG?AW4FMOD_RESULT@@PBDPAUFMOD_STUDIO_USER_PROPERTY@@@Z", click, gstr("stealing_policy_custom"), buf),
          74);
    uint32_t rain = GETEVENT("event:/music/rainforest/03");
    CHECK("getUserProperty(float)",
          FS(c, STU("getUserProperty@EventDescription") "QBG?AW4FMOD_RESULT@@PBDPAUFMOD_STUDIO_USER_PROPERTY@@@Z", rain, gstr("fade_out_speed_multiplier"), buf),
          0);
    CHECK("... {guest name, FLOAT, 2.5}", !strcmp((char *)P(rd32(buf)), "fade_out_speed_multiplier") && rd32(buf + 4) == 2 && rdf32(buf + 8) == 2.5f, 1);
    uint32_t boom = GETEVENT("event:/explosions/magic_rocket_small");
    CHECK("getMinimumDistance", FS(c, STU("getMinimumDistance@EventDescription") "QBG?AW4FMOD_RESULT@@PAM@Z", boom, out), 0);
    CHECK("getMaximumDistance", FS(c, STU("getMaximumDistance@EventDescription") "QBG?AW4FMOD_RESULT@@PAM@Z", boom, out + 4), 0);
    CHECK("3D event: max distance > min distance > 0", rdf32(out) > 0 && rdf32(out + 4) > rdf32(out), 1);
    CHECK("getLength", FS(c, STU("getLength@EventDescription") "QBG?AW4FMOD_RESULT@@PAH@Z", boom, out), 0);
    CHECK("loadSampleData", FS(c, STU("loadSampleData@EventDescription") "QAG?AW4FMOD_RESULT@@XZ", click), 0);

    // Buses (0x47a8e0..).
    CHECK("getBus(bus:/)", FS(c, STU("getBus@System") "QBG?AW4FMOD_RESULT@@PBDPAPAVBus@23@@Z", sys, gstr("bus:/"), out), 0);
    CHECK("bus setVolume", FS(c, STU("setVolume@Bus") "QAG?AW4FMOD_RESULT@@M@Z", rd32(out), 0x3f800000), 0);
    CHECK("bus setPaused(false)", FS(c, STU("setPaused@Bus") "QAG?AW4FMOD_RESULT@@_N@Z", rd32(out), 0), 0);
    uint32_t attrs = heap_calloc(12, 4);
    wrf32(attrs + 32, 1.0f), wrf32(attrs + 40, 1.0f);  // {position, velocity, forward (0,0,1), up (0,1,0)}
    CHECK("setListenerAttributes",
          FS(c, STU("setListenerAttributes@System") "QAG?AW4FMOD_RESULT@@HPBUFMOD_3D_ATTRIBUTES@@PBUFMOD_VECTOR@@@Z", sys, 0, attrs, 0), 0);

    // Instances with the game's callback: beats from a level track, a marker from the boss music, then
    // stop -> STOPPED -> the callback releases the instance -> DESTROYED retires its handle.
    const char *tracks[2] = {"event:/music/coalmine/03", "event:/music/boss_arena/battle"};
    uint32_t ei[2];
    EXPECT_USER = 0x12345678;
    for (int i = 0; i < 2; i++) {
        uint32_t desc = GETEVENT(tracks[i]);
        FS(c, STU("createInstance@EventDescription") "QBG?AW4FMOD_RESULT@@PAPAVEventInstance@23@@Z", desc, out);
        ei[i] = rd32(out);
        FS(c, STU("setUserData@EventInstance") "QAG?AW4FMOD_RESULT@@PAX@Z", ei[i], EXPECT_USER);
        CHECK("instance setCallback(all)",
              FS(c, STU("setCallback@EventInstance") "QAG?AW4FMOD_RESULT@@P6G?AW44@IPAUFMOD_STUDIO_EVENTINSTANCE@@PAX@ZI@Z", ei[i], G_EVCB, 0xffffffff), 0);
        CHECK("set3DAttributes", FS(c, STU("set3DAttributes@EventInstance") "QAG?AW4FMOD_RESULT@@PBUFMOD_3D_ATTRIBUTES@@@Z", ei[i], attrs), 0);
        CHECK("setVolume", FS(c, STU("setVolume@EventInstance") "QAG?AW4FMOD_RESULT@@M@Z", ei[i], 0x3f800000), 0);
        CHECK("start", FS(c, STU("start@EventInstance") "QAG?AW4FMOD_RESULT@@XZ", ei[i]), 0);
    }
    for (int t = 0; t < 5000 && !(BEATS >= 2 && MARKERS); t += 50) update(c, sys, 50);
    FS(c, STU("getTimelinePosition@EventInstance") "QBG?AW4FMOD_RESULT@@PAH@Z", ei[0], out);
    printf("  %d beats, %d markers (last \"%s\"), timeline position %d ms\n", BEATS, MARKERS, MARKER, rd32(out));
    CHECK("timeline advanced", (int32_t)rd32(out) > 0, 1);
    CHECK("beat callbacks with guest parameters", BEATS >= 2 && BEAT_OK == BEATS, 1);
    CHECK("marker callback with a guest name", MARKERS > 0 && !strcmp(MARKER, "start1"), 1);
    CHECK("setParameterByName(missing)",
          FS(c, STU("setParameterByName@EventInstance") "QAG?AW4FMOD_RESULT@@PBDM_N@Z", ei[0], gstr("no_such_parameter"), 0, 0) != 0, 1);
    CHECK("setPaused(true)", FS(c, STU("setPaused@EventInstance") "QAG?AW4FMOD_RESULT@@_N@Z", ei[0], 0xffffff01), 0);
    for (int i = 0; i < 2; i++)
        CHECK("stop", FS(c, STU("stop@EventInstance") "QAG?AW4FMOD_RESULT@@W4FMOD_STUDIO_STOP_MODE@@@Z", ei[i], 1), 0);  // immediate
    for (int t = 0; t < 2000 && (FS(c, EI_ISVALID, ei[0]) || FS(c, EI_ISVALID, ei[1])); t += 50) update(c, sys, 50);
    CHECK("STOPPED callbacks saw the user data and released", STOPS == 2 && STOP_USER_OK == 2 && RELEASED == 2, 1);
    CHECK("destroyed: handles invalid", (FS(c, EI_ISVALID, ei[0]) | FS(c, EI_ISVALID, ei[1])) & 0xff, 0);
    CHECK("destroyed: calls fail with FMOD_ERR_INVALID_HANDLE", FS(c, STU("start@EventInstance") "QAG?AW4FMOD_RESULT@@XZ", ei[0]), 30);
    CHECK("event callbacks on FMOD's thread", EV_CALLS > 0 && EV_OTHER_THREAD == EV_CALLS, 1);
    CHECK("system callbacks on FMOD's thread", SYS_CALLS > 0 && SYS_OTHER_THREAD == SYS_CALLS, 1);

    // A one-shot without a callback is retired too.
    FS(c, STU("createInstance@EventDescription") "QBG?AW4FMOD_RESULT@@PAPAVEventInstance@23@@Z", click, out);
    uint32_t shot = rd32(out);
    FS(c, STU("start@EventInstance") "QAG?AW4FMOD_RESULT@@XZ", shot);
    FS(c, EI_RELEASE, shot);
    for (int t = 0; t < 3000 && FS(c, EI_ISVALID, shot); t += 50) update(c, sys, 50);
    CHECK("released one-shot retired after it ends", FS(c, EI_ISVALID, shot) & 0xff, 0);

    CHECK("release", FS(c, STU("release@System") "QAG?AW4FMOD_RESULT@@XZ", sys), 0);
    CHECK("released system invalid", FS(c, STU("isValid@System") "QBG_NXZ", sys) & 0xff, 0);
    printf("%d/%d checks ok\n", checks - fails, checks);
    return fails != 0;
}
