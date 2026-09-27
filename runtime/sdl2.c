// The SDL2 bridge's runtime half (sdl2.h): handles, surface mirrors, returned strings, and the imports
// that don't fit tools/gen_sdl.py's generated thunks (SDL_PollEvent, SDL_FreeSurface).
#include "sdl2.h"

#include <fcntl.h>
#include <os/lock.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "heap.h"
#include "hle.h"

// Handle block header (guest memory, just below the handle): magic, kind, host pointer.
enum { HDR = 16, MAGIC = 0x484c4453 /* "SDLH" */ };
enum { K_OPAQUE, K_SURFACE, K_FORMAT, K_PALETTE };

static os_unfair_lock LOCK = OS_UNFAIR_LOCK_INIT;

// host pointer -> handle; open addressing, linear probing. A removed entry keeps its key with value 0.
typedef struct { uintptr_t key; uint32_t val; } Slot;
static Slot *MAP;
static size_t MAP_CAP, MAP_USED;

static Slot *slot_of(uintptr_t key) {
    size_t i = (key >> 4) * 0x9e3779b97f4a7c15ull >> 20;
    for (;; i++) {
        Slot *s = &MAP[i & (MAP_CAP - 1)];
        if (!s->key || s->key == key) return s;
    }
}

static uint32_t map_get(uintptr_t key) { return MAP ? slot_of(key)->val : 0; }  // lock held

static uint32_t lookup(void *h) {
    os_unfair_lock_lock(&LOCK);
    uint32_t g = map_get((uintptr_t)h);
    os_unfair_lock_unlock(&LOCK);
    return g;
}

static void map_put(uintptr_t key, uint32_t val) {
    if (2 * (MAP_USED + 1) > MAP_CAP) {
        Slot *old = MAP;
        size_t n = MAP_CAP;
        MAP_CAP = n ? 2 * n : 1024;
        MAP = calloc(MAP_CAP, sizeof *MAP);
        MAP_USED = 0;
        for (size_t i = 0; i < n; i++)
            if (old[i].val) *slot_of(old[i].key) = old[i], MAP_USED++;
        free(old);
    }
    Slot *s = slot_of(key);
    if (!s->key) MAP_USED++;
    *s = (Slot){key, val};
}

// The handle for h, allocating a `size`-byte zeroed block of `kind` if there is none. *made: new block.
static uint32_t handle_of(void *h, int kind, uint32_t size, int *made) {
    if (!h) return 0;
    os_unfair_lock_lock(&LOCK);
    uint32_t g = map_get((uintptr_t)h);
    *made = !g;
    if (!g) {
        uint32_t b = heap_calloc(1, HDR + size);
        if (!b) { fprintf(stderr, "sdl2: guest heap exhausted\n"); exit(6); }
        wr32(b, MAGIC), wr32(b + 4, kind), wr64(b + 8, (uintptr_t)h);
        g = b + HDR;
        map_put((uintptr_t)h, g);
    }
    os_unfair_lock_unlock(&LOCK);
    return g;
}

static int is_handle(uint32_t g, int kind) {  // kind -1: any
    return g >= HEAP_LO + HDR && heap_owns(g - HDR) && rd32(g - HDR) == MAGIC && (kind < 0 || (int)rd32(g - 12) == kind);
}

uint32_t sdl_handle(void *h) {
    int made;
    return handle_of(h, K_OPAQUE, 0, &made);
}

void *sdl_host_or_null(uint32_t g) { return is_handle(g, -1) ? (void *)(uintptr_t)rd64(g - 8) : NULL; }

void *sdl_host(uint32_t g) {
    if (g && !is_handle(g, -1)) { fprintf(stderr, "sdl2: %#x is not an SDL handle\n", g); exit(5); }
    return sdl_host_or_null(g);
}

uint32_t sdl_guest_str(uint32_t *buf, const char *s) {
    if (!s) return 0;
    uint32_t n = strlen(s) + 1;
    if (!*buf || heap_size(*buf) < n) *buf = heap_realloc(*buf, n);
    memcpy(P(*buf), s, n);
    return *buf;
}

static uint32_t gaddr(const void *p) {  // guest address of a host pointer into guest memory (NULL: 0)
    return p ? (uint32_t)((const uint8_t *)p - MEM) : 0;
}
static int in_guest(const void *p) { return (const uint8_t *)p >= MEM && (const uint8_t *)p < MEM + (1ull << 32); }

// Palettes and formats are shared and refcounted by SDL; their mirrors live as long as the process.
static uint32_t palette_guest(SDL_Palette *p) {
    int made;
    uint32_t g = handle_of(p, K_PALETTE, G_SDL_Palette_SIZE, &made);
    if (!g) return 0;
    uint32_t colors = rd32(g + G_SDL_Palette_colors);
    if ((int)rd32(g + G_SDL_Palette_ncolors) != p->ncolors || !colors)
        colors = heap_realloc(colors, 4 * p->ncolors);  // SDL_Color: 4 bytes on both sides
    memcpy(P(colors), p->colors, 4 * p->ncolors);
    wr32(g + G_SDL_Palette_ncolors, p->ncolors);
    wr32(g + G_SDL_Palette_colors, colors);
    wr32(g + G_SDL_Palette_version, p->version);
    wr32(g + G_SDL_Palette_refcount, p->refcount);
    return g;
}

static uint32_t format_guest(SDL_PixelFormat *f) {
    int made;
    uint32_t g = handle_of(f, K_FORMAT, G_SDL_PixelFormat_SIZE, &made);
    if (!g) return 0;
    wr32(g + G_SDL_PixelFormat_format, f->format);
    wr32(g + G_SDL_PixelFormat_palette, palette_guest(f->palette));
    // BitsPerPixel .. Ashift: 28 bytes of Uint8/Uint32 fields, laid out identically on both sides.
    _Static_assert(G_SDL_PixelFormat_refcount - G_SDL_PixelFormat_BitsPerPixel ==
                   offsetof(SDL_PixelFormat, refcount) - offsetof(SDL_PixelFormat, BitsPerPixel), "SDL_PixelFormat");
    memcpy(P(g + G_SDL_PixelFormat_BitsPerPixel), &f->BitsPerPixel,
           G_SDL_PixelFormat_refcount - G_SDL_PixelFormat_BitsPerPixel);
    wr32(g + G_SDL_PixelFormat_refcount, f->refcount);
    return g;
}

void sdl_surface_sync(SDL_Surface *s) {
    if (!s) return;
    uint32_t g = lookup(s);
    if (!g) return;
    wr32(g + G_SDL_Surface_flags, s->flags);
    wr32(g + G_SDL_Surface_format, format_guest(s->format));
    wr32(g + G_SDL_Surface_w, s->w);
    wr32(g + G_SDL_Surface_h, s->h);
    wr32(g + G_SDL_Surface_pitch, s->pitch);
    wr32(g + G_SDL_Surface_pixels, gaddr(s->pixels));
    wr32(g + G_SDL_Surface_locked, s->locked);
    memcpy(P(g + G_SDL_Surface_clip_rect), &s->clip_rect, sizeof s->clip_rect);
    wr32(g + G_SDL_Surface_refcount, s->refcount);
}

// An equivalent of s whose pixels are in the guest heap (s itself if they already are in guest memory).
// The new surface's host userdata holds the guest pixel block, which SDL_FreeSurface frees.
static SDL_Surface *to_guest_pixels(SDL_Surface *s) {
    if (!s->pixels || in_guest(s->pixels)) return s;
    uint32_t n = (uint32_t)s->pitch * s->h, px = heap_alloc(n);
    SDL_Surface *d = px ? SDL_CreateRGBSurfaceWithFormatFrom(P(px), s->w, s->h, s->format->BitsPerPixel, s->pitch,
                                                              s->format->format) : NULL;
    if (!d) { fprintf(stderr, "sdl2: can't move a %dx%d surface to guest memory: %s\n", s->w, s->h, SDL_GetError()); exit(6); }
    SDL_LockSurface(s);
    memcpy(P(px), s->pixels, n);
    SDL_UnlockSurface(s);
    if (s->format->palette) SDL_SetSurfacePalette(d, s->format->palette);
    Uint32 key;
    if (!SDL_GetColorKey(s, &key)) SDL_SetColorKey(d, SDL_TRUE, key);
    SDL_BlendMode blend;
    Uint8 r, gr, b, a;
    SDL_GetSurfaceBlendMode(s, &blend), SDL_SetSurfaceBlendMode(d, blend);
    SDL_GetSurfaceColorMod(s, &r, &gr, &b), SDL_SetSurfaceColorMod(d, r, gr, b);
    SDL_GetSurfaceAlphaMod(s, &a), SDL_SetSurfaceAlphaMod(d, a);
    SDL_SetClipRect(d, &s->clip_rect);
    d->userdata = (void *)(uintptr_t)px;
    SDL_FreeSurface(s);
    return d;
}

uint32_t sdl_surface_guest(SDL_Surface *s) {
    if (!s) return 0;
    s = to_guest_pixels(s);
    int made;
    uint32_t g = handle_of(s, K_SURFACE, G_SDL_Surface_SIZE, &made);
    sdl_surface_sync(s);
    return g;
}

SDL_Surface *sdl_surface_host(uint32_t g) {
    if (g && !is_handle(g, K_SURFACE)) { fprintf(stderr, "sdl2: %#x is not an SDL_Surface\n", g); exit(5); }
    return sdl_host_or_null(g);
}

HOST_CDECL(SDL2, SDL_FreeSurface) {  // void SDL_FreeSurface(SDL_Surface *surface)
    uint32_t g = ARG(0);
    SDL_Surface *s = sdl_surface_host(g);
    if (!s) return;
    if (s->refcount > 1) {
        SDL_FreeSurface(s);
        return sdl_surface_sync(s);
    }
    uint32_t px = (uint32_t)(uintptr_t)s->userdata;
    os_unfair_lock_lock(&LOCK);
    map_put((uintptr_t)s, 0);
    os_unfair_lock_unlock(&LOCK);
    wr32(g - HDR, 0);
    heap_free(g - HDR);
    SDL_FreeSurface(s);
    heap_free(px);
}

// Fullscreen as on Windows: the game leaves fullscreen with SDL_SetWindowSize then SDL_SetWindowFullscreen(0)
// and sets its GL viewport right away. A macOS fullscreen Space leaves asynchronously and then restores its
// own (screen-sized) frame, so the game drew 1280x720 into the corner of a huge window. Without Spaces,
// SDL switches synchronously and restores the size the game asked for.
__attribute__((constructor)) static void sdl_hints(void) {
    SDL_SetHintWithPriority(SDL_HINT_VIDEO_MAC_FULLSCREEN_SPACES, "0", SDL_HINT_DEFAULT);
}

// SDL_Event is 56 bytes on both sides and most members are laid out identically; tools/gen_sdl.py checks
// that the ones below are the only exceptions (they hold pointers).
static void event_to_guest(SDL_Event *e, uint32_t g) {
    memcpy(P(g), e, G_SDL_Event_SIZE);
    switch (e->type) {
    case SDL_DROPFILE: case SDL_DROPTEXT: case SDL_DROPBEGIN: case SDL_DROPCOMPLETE:
        // The guest frees the file with SDL_free (sdl2_stdlib.c: the guest heap).
        wr32(g + G_SDL_DropEvent_file, e->drop.file ? guest_strdup(e->drop.file) : 0);
        wr32(g + G_SDL_DropEvent_windowID, e->drop.windowID);
        SDL_free(e->drop.file);
        break;
    case SDL_TEXTEDITING_EXT: {
        static uint32_t buf;
        wr32(g + G_SDL_TextEditingExtEvent_text, sdl_guest_str(&buf, e->editExt.text));
        wr32(g + G_SDL_TextEditingExtEvent_start, e->editExt.start);
        wr32(g + G_SDL_TextEditingExtEvent_length, e->editExt.length);
        SDL_free(e->editExt.text);
        break;
    }
    case SDL_SYSWMEVENT:
        wr32(g + G_SDL_SysWMEvent_msg, 0);  // host window-system messages mean nothing to the guest
        break;
    default:
        if (e->type >= SDL_USEREVENT && e->type <= SDL_LASTEVENT) {
            wr32(g + G_SDL_UserEvent_data1, sdl_handle(e->user.data1));
            wr32(g + G_SDL_UserEvent_data2, sdl_handle(e->user.data2));
        }
    }
}

// Scripted input for driving the game without a person (the game reads the mouse only through events):
// MINA4MAC_CLICKS="t:x,y[;t:x,y...]" left-clicks at window point (x, y), t seconds after the first
// SDL_PollEvent: the pointer moves there, the button goes down 100 ms later and comes up 100 ms after that.
static void scripted_input(void) {
    static const char *next;
    static uint32_t t0, up;  // up: when a pending click's release is due
    if (!t0) {
        t0 = SDL_GetTicks() | 1;
        next = getenv("MINA4MAC_CLICKS");
    }
    SDL_Window *w = SDL_GL_GetCurrentWindow();
    SDL_Event e = {0};
    e.button.windowID = w ? SDL_GetWindowID(w) : 0;
    e.button.button = SDL_BUTTON_LEFT;
    static int x, y;
    static uint32_t down;  // when a pending click's press is due (the pointer moves there a few frames earlier)
    if (down) {
        if (SDL_GetTicks() < down) return;
        down = 0;
        e.type = SDL_MOUSEBUTTONDOWN, e.button.x = x, e.button.y = y, e.button.state = SDL_PRESSED, e.button.clicks = 1;
        SDL_PushEvent(&e);
        up = SDL_GetTicks() + 100;
        return;
    }
    if (up) {
        if (SDL_GetTicks() < up) return;
        up = 0;
        e.type = SDL_MOUSEBUTTONUP, e.button.x = x, e.button.y = y;
        SDL_PushEvent(&e);
        return;
    }
    double t;
    if (!next || !*next || sscanf(next, "%lf:%d,%d", &t, &x, &y) != 3 || SDL_GetTicks() - t0 < t * 1000) return;
    next = strchr(next, ';') ? strchr(next, ';') + 1 : "";
    e.type = SDL_MOUSEMOTION, e.motion.x = x, e.motion.y = y;
    SDL_PushEvent(&e);
    down = SDL_GetTicks() + 100;  // some widgets (dialog buttons) only take a press they were hovered before
}

// MINA4MAC_TRACE=...,SDL_WINDOWEVENT logs the window events the game receives, with the window's size,
// drawable size and flags at that moment.
static void trace_window_event(const SDL_Event *e) {
    SDL_Window *w = SDL_GetWindowFromID(e->window.windowID);
    int ww = 0, wh = 0, dw = 0, dh = 0;
    if (w) SDL_GetWindowSize(w, &ww, &wh), SDL_GL_GetDrawableSize(w, &dw, &dh);
    fprintf(stderr, "[sdl] %u SDL_WINDOWEVENT %d (%d, %d): window %dx%d, drawable %dx%d, flags %#x\n", SDL_GetTicks(),
            e->window.event, e->window.data1, e->window.data2, ww, wh, dw, dh, w ? SDL_GetWindowFlags(w) : 0);
}

HOST_CDECL(SDL2, SDL_PollEvent) {  // int SDL_PollEvent(SDL_Event *event)
    scripted_input();
    SDL_Event e;
    static int trace = -1;
    if (trace < 0) trace = rt_traced("SDL2", "SDL_WINDOWEVENT");
    int r = SDL_PollEvent(ARG(0) ? &e : NULL);
    if (r && e.type == SDL_WINDOWEVENT && ARG(0) && trace) trace_window_event(&e);
    if (r && ARG(0)) event_to_guest(&e, ARG(0));
    ret_i32(c, r);
}

// MINA4MAC_FPS=1 counts frames at the swap: once a second, the frame rate and the average and worst frame
// time (swap to swap, so vsync caps it at the display's refresh rate), on stderr and in the window title.
static void count_frame(SDL_Window *w) {
    static int on = -1, frames;
    static uint64_t last, t0;
    static double worst;
    static char title[256];
    if (on < 0) on = getenv("MINA4MAC_FPS") && strcmp(getenv("MINA4MAC_FPS"), "0");
    if (!on) return;
    uint64_t now = SDL_GetPerformanceCounter(), hz = SDL_GetPerformanceFrequency();
    if (!last) {
        last = t0 = now;
        return;
    }
    double ms = (now - last) * 1000.0 / hz;
    last = now, frames++;
    if (ms > worst) worst = ms;
    double span = (double)(now - t0) / hz;
    if (span < 1) return;
    double fps = frames / span;
    fprintf(stderr, "[fps] %.1f fps, frame avg %.2f ms, worst %.2f ms\n", fps, span * 1000 / frames, worst);
    if (w) {
        const char *cur = SDL_GetWindowTitle(w);
        if (!title[0] || strncmp(cur, title, strlen(title))) {  // the game's own title, before our suffix
            const char *bar = strstr(cur, " | ");
            snprintf(title, sizeof title, "%.*s", bar ? (int)(bar - cur) : (int)strlen(cur), cur);
        }
        char buf[320];
        snprintf(buf, sizeof buf, "%s | %.0f fps, worst %.1f ms", title, fps, worst);
        SDL_SetWindowTitle(w, buf);
    }
    t0 = now, frames = 0, worst = 0;
}

// MINA4MAC_FRAMELOG=<file> appends one line per frame, "f <work ms> <cpu ms> <swap ms>": work is the main thread's
// wall time from the previous swap's return to this swap (the frame's CPU side, including waits for job workers,
// excluding swap and vsync), cpu its thread CPU time over the same span, swap the time inside SDL_GL_SwapWindow.
// O_APPEND, one write per line, so other writers of the file (perfbench's start/end marks) interleave in order.
static double now_ms(clockid_t id) {
    struct timespec ts;
    clock_gettime(id, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

HOST_CDECL(SDL2, SDL_GL_SwapWindow) {  // void SDL_GL_SwapWindow(SDL_Window *window)
    static int fd = -2;
    static double wall0, cpu0;
    if (fd == -2) {
        const char *path = getenv("MINA4MAC_FRAMELOG");
        fd = path && *path ? open(path, O_WRONLY | O_CREAT | O_APPEND, 0644) : -1;
    }
    double wall = 0, cpu = 0;
    if (fd >= 0) wall = now_ms(CLOCK_MONOTONIC_RAW), cpu = now_ms(CLOCK_THREAD_CPUTIME_ID);
    SDL_Window *w = sdl_host(ARG(0));
    SDL_GL_SwapWindow(w);
    count_frame(w);
    if (fd >= 0) {
        double after = now_ms(CLOCK_MONOTONIC_RAW);
        if (wall0) {
            char line[64];
            int n = snprintf(line, sizeof line, "f %.3f %.3f %.3f\n", wall - wall0, cpu - cpu0, after - wall);
            if (write(fd, line, n) < 0) close(fd), fd = -1;
        }
        wall0 = after, cpu0 = now_ms(CLOCK_THREAD_CPUTIME_ID);
    }
}
