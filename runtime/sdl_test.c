// SDL2 bridge test (built and run by tools/check.sh): call the generated SDL2 thunks (sdl2_gen.c) and
// the hand-written ones (sdl2.c) the way guest code does, through their import thunks, against the host
// SDL2: SDL_GetVersion and SDL_GetTicks, then strings, handles, surface mirrors and events.
//   sdl_test
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "heap.h"
#include "hle.h"
#include "sdl2.h"

const FnEntry FN_TABLE[1];
const int FN_COUNT = 0;
enum { RET_ADDR = 0x0badf000, STACK_TOP = 0x10000000 };

static int fails;
#define CHECK(name, got, want)                                                                        \
    do {                                                                                              \
        uint64_t g_ = (got), w_ = (want);                                                             \
        printf("%-36s %s got %#llx want %#llx\n", name, g_ == w_ ? "ok  " : "FAIL", g_, w_);         \
        fails += g_ != w_;                                                                            \
    } while (0)

// Guest-side call of SDL2.dll!name with 4-byte args; returns eax. *popped: bytes the callee popped.
static uint32_t popped;
static uint32_t call(CPU *c, const char *name, int n, const uint32_t *args) {
    uint32_t top = c->esp;
    for (int i = n - 1; i >= 0; i--) { c->esp -= 4; wr32(c->esp, args[i]); }
    c->esp -= 4;
    wr32(c->esp, RET_ADDR);
    guest_call(c, rt_thunk("SDL2.dll", name));
    popped = c->esp - (top - 4 * n - 4);
    c->esp = top;
    return c->eax;
}
#define SDL(name, ...) call(&c, name, sizeof((uint32_t[]){__VA_ARGS__}) / 4, (uint32_t[]){__VA_ARGS__})
#define SDL0(name) call(&c, name, 0, NULL)

static uint32_t gs(const char *s) { return guest_strdup(s); }
static int gstreq(uint32_t p, const char *s) { return p && !strcmp((char *)P(p), s); }

int main(void) {
    rt_init();
    CPU c = {.esp = STACK_TOP, .fpu_cw = 0x27f};

    // SDL_GetVersion(SDL_version *) into guest memory, and SDL_GetTicks, as the task asks.
    uint32_t ver = heap_calloc(1, 4);
    SDL("SDL_GetVersion", ver);
    SDL_version hv;
    SDL_GetVersion(&hv);
    CHECK("GetVersion major", rd8(ver), hv.major);
    CHECK("GetVersion minor", rd8(ver + 1), hv.minor);
    CHECK("GetVersion patch", rd8(ver + 2), hv.patch);
    CHECK("GetVersion is 2.x", rd8(ver), 2);
    CHECK("cdecl: pops only the return", popped, 4);
    uint32_t t0 = SDL0("SDL_GetTicks");
    SDL("SDL_Delay", 30);
    uint32_t t1 = SDL0("SDL_GetTicks"), host = SDL_GetTicks();
    CHECK("GetTicks advances over Delay(30)", t1 - t0 >= 30 && t1 - t0 < 1000, 1);
    CHECK("GetTicks matches host", host - t1 <= 1, 1);

    CHECK("Init(EVENTS)", SDL("SDL_Init", SDL_INIT_EVENTS), 0);

    // Returned strings are copied into guest memory.
    SDL_SetError("boom %d", 42);
    CHECK("GetError: guest string", gstreq(SDL0("SDL_GetError"), "boom 42"), 1);
    CHECK("GetKeyName(SDLK_a)", gstreq(SDL("SDL_GetKeyName", SDLK_a), "A"), 1);

    // Surfaces: created by SDL (pixels moved into the guest heap), or from guest pixels.
    uint32_t s = SDL("SDL_CreateRGBSurface", 0, 33, 7, 32, 0xff0000, 0xff00, 0xff, 0xff000000);
    CHECK("CreateRGBSurface: mirror", s != 0, 1);
    CHECK("  w", rd32(s + G_SDL_Surface_w), 33);
    CHECK("  h", rd32(s + G_SDL_Surface_h), 7);
    CHECK("  pitch", rd32(s + G_SDL_Surface_pitch), 132);
    uint32_t px = rd32(s + G_SDL_Surface_pixels), fmt = rd32(s + G_SDL_Surface_format);
    CHECK("  pixels in guest heap", heap_owns(px), 1);
    CHECK("  format BytesPerPixel", rd8(fmt + G_SDL_PixelFormat_BytesPerPixel), 4);
    CHECK("  format Rmask", rd32(fmt + G_SDL_PixelFormat_Rmask), 0xff0000);
    CHECK("  format Ashift", rd8(fmt + G_SDL_PixelFormat_Ashift), 24);
    CHECK("  clip_rect.w", rd32(s + G_SDL_Surface_clip_rect + 8), 33);
    CHECK("  host surface shares pixels", sdl_surface_host(s)->pixels == P(px), 1);
    CHECK("LockSurface", SDL("SDL_LockSurface", s), 0);
    CHECK("  mirror locked", rd32(s + G_SDL_Surface_locked), 1);
    SDL("SDL_UnlockSurface", s);
    CHECK("  mirror unlocked", rd32(s + G_SDL_Surface_locked), 0);
    uint32_t key = heap_alloc(4);
    CHECK("GetColorKey: none", SDL("SDL_GetColorKey", s, key), (uint32_t)-1);

    uint32_t gpx = heap_calloc(16 * 4, 4);
    for (int i = 0; i < 16; i++) wr32(gpx + 4 * i, 0xff000000u | i * 0x010203);
    uint32_t src = SDL("SDL_CreateRGBSurfaceFrom", gpx, 4, 4, 32, 16, 0xff0000, 0xff00, 0xff, 0xff000000);
    CHECK("CreateRGBSurfaceFrom: guest pixels", rd32(src + G_SDL_Surface_pixels), gpx);
    uint32_t dst_rect = heap_calloc(4, 4);
    wr32(dst_rect, 10), wr32(dst_rect + 4, 2);
    CHECK("UpperBlit", SDL("SDL_UpperBlit", src, 0, s, dst_rect), 0);
    CHECK("  pixel (11,3) in guest memory", rd32(px + 3 * 132 + 11 * 4), 0xff000000u | 5 * 0x010203);
    CHECK("  dstrect clipped w", rd32(dst_rect + 8), 4);

    // A BMP through a guest RWops handle, with a Windows-style relative path.
    SDL_Surface *h = SDL_CreateRGBSurface(0, 5, 3, 24, 0xff0000, 0xff00, 0xff, 0);
    ((uint8_t *)h->pixels)[h->pitch * 2 + 3 * 4] = 0x5a;
    char dir[4096];
    snprintf(dir, sizeof dir, "/tmp/noitamac_sdltest_%d", getpid());
    mkdir(dir, 0755);
    char bmp[4200];
    snprintf(bmp, sizeof bmp, "%s/icon.bmp", dir);
    SDL_SaveBMP(h, bmp);
    SDL_FreeSurface(h);
    if (chdir(dir)) perror(dir);
    uint32_t rw = SDL("SDL_RWFromFile", gs(".\\icon.bmp"), gs("rb"));
    CHECK("RWFromFile: handle", rw != 0 && sdl_host(rw) != NULL, 1);
    uint32_t bs = SDL("SDL_LoadBMP_RW", rw, 1);
    CHECK("LoadBMP_RW: mirror", bs != 0, 1);
    CHECK("  size", rd32(bs + G_SDL_Surface_w) << 8 | rd32(bs + G_SDL_Surface_h), 5 << 8 | 3);
    uint32_t bpx = rd32(bs + G_SDL_Surface_pixels);
    CHECK("  pixels moved to guest heap", heap_owns(bpx), 1);
    CHECK("  pixel content", rd8(bpx + rd32(bs + G_SDL_Surface_pitch) * 2 + 3 * 4), 0x5a);
    unlink(bmp);
    rmdir(dir);
    SDL("SDL_FreeSurface", bs);
    CHECK("FreeSurface: mirror freed", heap_owns(bs - 16), 0);
    CHECK("  guest pixels freed", heap_owns(bpx), 0);
    SDL("SDL_FreeSurface", src);
    CHECK("  guest-owned pixels kept", heap_owns(gpx), 1);
    SDL("SDL_FreeSurface", s);
    SDL("SDL_FreeSurface", 0);

    // Events: identical members copied, pointer members converted.
    int marker;
    SDL_Event e = {.user = {.type = SDL_USEREVENT, .code = 7, .data1 = &marker}};
    SDL_PushEvent(&e);
    e = (SDL_Event){.drop = {.type = SDL_DROPFILE, .file = SDL_strdup("Z:\\a.txt"), .windowID = 9}};
    SDL_PushEvent(&e);
    e = (SDL_Event){.key = {.type = SDL_KEYDOWN, .windowID = 3, .keysym = {.scancode = SDL_SCANCODE_A, .sym = SDLK_a}}};
    SDL_PushEvent(&e);
    uint32_t ev = heap_calloc(1, G_SDL_Event_SIZE);
    CHECK("PollEvent: user", SDL("SDL_PollEvent", ev), 1);
    CHECK("  type", rd32(ev), SDL_USEREVENT);
    CHECK("  code", rd32(ev + G_SDL_UserEvent_code), 7);
    CHECK("  data1 handle -> host pointer", sdl_host(rd32(ev + G_SDL_UserEvent_data1)) == &marker, 1);
    CHECK("  data2 NULL", rd32(ev + G_SDL_UserEvent_data2), 0);
    CHECK("PollEvent: drop", SDL("SDL_PollEvent", ev), 1);
    CHECK("  file in guest heap", gstreq(rd32(ev + G_SDL_DropEvent_file), "Z:\\a.txt"), 1);
    CHECK("  windowID", rd32(ev + G_SDL_DropEvent_windowID), 9);
    SDL("SDL_free", rd32(ev + G_SDL_DropEvent_file));
    CHECK("PollEvent: key", SDL("SDL_PollEvent", ev), 1);
    CHECK("  windowID", rd32(ev + G_SDL_KeyboardEvent_windowID), 3);
    CHECK("  keysym.sym", rd32(ev + G_SDL_KeyboardEvent_keysym + 4), SDLK_a);
    CHECK("PollEvent(NULL): empty", SDL("SDL_PollEvent", 0), 0);

    // Display modes (SDL_DisplayMode differs: driverdata is a pointer). Needs the video subsystem.
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) == 0) {
        uint32_t m = heap_alloc(G_SDL_DisplayMode_SIZE);
        memset(P(m), 0xcc, G_SDL_DisplayMode_SIZE);  // garbage in, as from an uninitialised local
        SDL_DisplayMode hm;
        SDL_GetDesktopDisplayMode(0, &hm);
        CHECK("GetDesktopDisplayMode", SDL("SDL_GetDesktopDisplayMode", 0, m), 0);
        CHECK("  w", rd32(m + G_SDL_DisplayMode_w), hm.w);
        CHECK("  h", rd32(m + G_SDL_DisplayMode_h), hm.h);
        CHECK("  format", rd32(m + G_SDL_DisplayMode_format), hm.format);
        CHECK("  driverdata handle", sdl_host_or_null(rd32(m + G_SDL_DisplayMode_driverdata)) == hm.driverdata, 1);
        CHECK("GetNumVideoDisplays", SDL0("SDL_GetNumVideoDisplays"), SDL_GetNumVideoDisplays());
    } else {
        printf("(video unavailable, display mode checks skipped: %s)\n", SDL_GetError());
    }
    SDL0("SDL_Quit");

    printf("sdltest: %s (%d failed)\n", fails ? "FAIL" : "ok", fails);
    return fails != 0;
}
