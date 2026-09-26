// OpenGL bridge test (built and run by tools/check.sh as gltest): call the opengl32 thunks (generated
// gl_gen.c and hand-written opengl32.c) the way guest code does, with guest memory for every pointer,
// against the host's legacy GL context of a hidden SDL window. Draws into a framebuffer object and reads the
// pixels back: clear, a triangle from guest client arrays, one from a vertex buffer with guest indices; plus
// strings, GetProcAddress, buffer mapping, pixel buffer offsets, shaders and sync objects.
//   gl_test
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <stdio.h>
#include <string.h>

#include "heap.h"
#include "opengl32.h"

const FnEntry FN_TABLE[1];
const int FN_COUNT = 0;
enum { RET_ADDR = 0x0badf000, STACK_TOP = 0x10000000, W = 64, H = 64 };

static int fails;
#define CHECK(name, got, want)                                                                        \
    do {                                                                                              \
        uint64_t g_ = (got), w_ = (want);                                                             \
        printf("%-44s %s got %#llx want %#llx\n", name, g_ == w_ ? "ok  " : "FAIL", g_, w_);         \
        fails += g_ != w_;                                                                            \
    } while (0)

// Guest-side __stdcall of opengl32.dll!name with 4-byte stack slots; returns eax. popped: bytes popped,
// including the return address.
static CPU c = {.esp = STACK_TOP, .fpu_cw = 0x27f};
static uint32_t popped;
static uint32_t call(const char *name, int n, const uint32_t *args) {
    uint32_t top = c.esp;
    for (int i = n - 1; i >= 0; i--) { c.esp -= 4; wr32(c.esp, args[i]); }
    c.esp -= 4;
    wr32(c.esp, RET_ADDR);
    guest_call(&c, rt_thunk("opengl32.dll", name));
    popped = c.esp - (top - 4 * n - 4);
    c.esp = top;
    return c.eax;
}
#define GL(name, ...) call(#name, sizeof((uint32_t[]){__VA_ARGS__}) / 4, (uint32_t[]){__VA_ARGS__})
#define GL0(name) call(#name, 0, NULL)
static uint32_t f(float x) { uint32_t u; memcpy(&u, &x, 4); return u; }
static uint32_t lo(double x) { uint64_t u; memcpy(&u, &x, 8); return (uint32_t)u; }
static uint32_t hi(double x) { uint64_t u; memcpy(&u, &x, 8); return (uint32_t)(u >> 32); }

// Guest copy of n bytes.
static uint32_t gdup(const void *p, uint32_t n) { uint32_t g = heap_alloc(n); memcpy(P(g), p, n); return g; }
static uint32_t PIX;  // guest buffer for one RGBA pixel
static uint32_t pixel(int x, int y) {
    GL(glReadPixels, x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, PIX);
    return rd32(PIX);  // bytes R, G, B, A
}

int main(void) {
    rt_init();
    PIX = heap_calloc(1, 4);

    // No context yet: calls do nothing, results are 0, and __stdcall arguments are still popped.
    CHECK("no context: glClear pops ret + 4", (GL(glClear, GL_COLOR_BUFFER_BIT), popped), 8);
    CHECK("no context: glGetError 0", GL0(glGetError), 0);
    CHECK("no context: glGetString 0", GL(glGetString, GL_VERSION), 0);
    CHECK("no context: glCreateShader 0", GL(glCreateShader, GL_VERTEX_SHADER), 0);

    // GetProcAddress policy: bridged names get their thunk, names macOS's legacy context lacks get NULL.
    CHECK("proc glClear: thunk", gl_proc_address("glClear"), rt_thunk("opengl32.dll", "glClear"));
    CHECK("proc glShaderSource: thunk", gl_proc_address("glShaderSource") != 0, 1);
    CHECK("proc glGenVertexArrays (3.0): NULL", gl_proc_address("glGenVertexArrays"), 0);
    CHECK("proc glDebugMessageCallback (4.3): NULL", gl_proc_address("glDebugMessageCallback"), 0);
    CHECK("proc wglGetProcAddress: thunk", gl_proc_address("wglGetProcAddress") != 0, 1);
    CHECK("wglGetProcAddress(glClear)", GL(wglGetProcAddress, guest_strdup("glClear")), gl_proc_address("glClear"));
    CHECK("wglGetProcAddress(glGenVertexArrays): NULL", GL(wglGetProcAddress, guest_strdup("glGenVertexArrays")), 0);
    CHECK("wglGetProcAddress(wglSwapIntervalEXT): NULL", GL(wglGetProcAddress, guest_strdup("wglSwapIntervalEXT")), 0);

    if (SDL_Init(SDL_INIT_VIDEO)) { printf("SDL_Init: %s\n", SDL_GetError()); return 1; }
    SDL_Window *win = SDL_CreateWindow("gltest", 0, 0, W, H, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    SDL_GLContext ctx = win ? SDL_GL_CreateContext(win) : NULL;
    if (!ctx) { printf("no GL context: %s\n", SDL_GetError()); return 1; }

    uint32_t ver = GL(glGetString, GL_VERSION);
    CHECK("glGetString(VERSION): guest 2.1 string", ver && !strncmp((char *)P(ver), "2.1", 3), 1);
    CHECK("glGetString cached", GL(glGetString, GL_VERSION), ver);
    uint32_t glsl = GL(glGetString, GL_SHADING_LANGUAGE_VERSION);
    CHECK("glGetString(GLSL): 1.20", glsl && !strncmp((char *)P(glsl), "1.20", 4), 1);

    // A framebuffer object with an RGBA texture, all through thunks.
    uint32_t ids = heap_calloc(4, 4);
    GL(glGenTextures, 1, ids);
    uint32_t tex = rd32(ids);
    GL(glBindTexture, GL_TEXTURE_2D, tex);
    GL(glTexParameteri, GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    GL(glTexImage2D, GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
    GL(glGenFramebuffers, 1, ids);
    GL(glBindFramebuffer, GL_FRAMEBUFFER, rd32(ids));
    GL(glFramebufferTexture2D, GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    CHECK("framebuffer complete", GL(glCheckFramebufferStatus, GL_FRAMEBUFFER), GL_FRAMEBUFFER_COMPLETE);
    GL(glViewport, 0, 0, W, H);
    GL(glMatrixMode, GL_PROJECTION);
    GL0(glLoadIdentity);
    GL(glOrtho, lo(0), hi(0), lo(W), hi(W), lo(0), hi(0), lo(H), hi(H), lo(-1), hi(-1), lo(1), hi(1));
    CHECK("glOrtho (6 doubles) pops ret + 48", popped, 52);
    GL(glMatrixMode, GL_MODELVIEW);
    GL0(glLoadIdentity);

    GL(glClearColor, f(1), f(0), f(0), f(1));
    GL(glClear, GL_COLOR_BUFFER_BIT);
    CHECK("clear red", pixel(5, 5), 0xff0000ff);

    // A triangle from guest client arrays (no buffer bound: pointers are guest addresses).
    float tri[] = {0, 0, W, 0, 0, H};  // lower-left half
    uint8_t col[] = {0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255};
    GL(glEnableClientState, GL_VERTEX_ARRAY);
    GL(glEnableClientState, GL_COLOR_ARRAY);
    uint32_t gtri = gdup(tri, sizeof tri);
    GL(glVertexPointer, 2, GL_FLOAT, 0, gtri);
    GL(glColorPointer, 4, GL_UNSIGNED_BYTE, 0, gdup(col, sizeof col));
    GL(glDrawArrays, GL_TRIANGLES, 0, 3);
    CHECK("client-array triangle: green inside", pixel(5, 5), 0xff00ff00);
    CHECK("  red outside", pixel(W - 5, H - 5), 0xff0000ff);
    uint32_t gp = heap_calloc(1, 4);
    GL(glGetPointerv, GL_VERTEX_ARRAY_POINTER, gp);
    CHECK("glGetPointerv: the guest pointer", rd32(gp), gtri);

    // A triangle from a vertex buffer (the pointer is an offset) with guest-memory indices.
    float quad[] = {0, 0, 0, 0, W, H, 0, H};  // two dummy floats first, so the offset is 8
    uint8_t blue[] = {0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255};
    GL(glGenBuffers, 1, ids);
    GL(glBindBuffer, GL_ARRAY_BUFFER, rd32(ids));
    GL(glBufferData, GL_ARRAY_BUFFER, sizeof quad, gdup(quad, sizeof quad), GL_STATIC_DRAW);
    GL(glVertexPointer, 2, GL_FLOAT, 0, 8);
    GL(glBindBuffer, GL_ARRAY_BUFFER, 0);
    GL(glColorPointer, 4, GL_UNSIGNED_BYTE, 0, gdup(blue, sizeof blue));
    uint16_t idx[] = {0, 1, 2};  // (0,0) (W,H) (0,H): upper-left half
    GL(glDrawElements, GL_TRIANGLES, 3, GL_UNSIGNED_SHORT, gdup(idx, sizeof idx));
    CHECK("buffer-offset triangle: blue upper left", pixel(5, H - 5), 0xffff0000);
    CHECK("  lower right still red", pixel(W - 5, 5), 0xff0000ff);
    GL(glGetPointerv, GL_VERTEX_ARRAY_POINTER, gp);
    CHECK("glGetPointerv: the offset", rd32(gp), 8);
    GL(glDisableClientState, GL_VERTEX_ARRAY);
    GL(glDisableClientState, GL_COLOR_ARRAY);

    // glMapBuffer: a guest shadow copy, read from and written back to the buffer.
    uint32_t words[4] = {1, 2, 3, 4}, back = heap_calloc(4, 4);
    GL(glGenBuffers, 1, ids);
    GL(glBindBuffer, GL_ARRAY_BUFFER, rd32(ids));
    GL(glBufferData, GL_ARRAY_BUFFER, 16, gdup(words, 16), GL_DYNAMIC_DRAW);
    uint32_t m = GL(glMapBuffer, GL_ARRAY_BUFFER, GL_READ_WRITE);
    CHECK("glMapBuffer: guest heap shadow", heap_owns(m), 1);
    CHECK("  holds the contents", rd32(m + 12), 4);
    uint32_t mp = heap_calloc(1, 4);
    GL(glGetBufferPointerv, GL_ARRAY_BUFFER, GL_BUFFER_MAP_POINTER, mp);
    CHECK("glGetBufferPointerv: the shadow", rd32(mp), m);
    wr32(m + 4, 0xdeadbeef);
    CHECK("glUnmapBuffer", GL(glUnmapBuffer, GL_ARRAY_BUFFER), 1);
    GL(glGetBufferSubData, GL_ARRAY_BUFFER, 0, 16, back);
    CHECK("  write copied back", rd32(back + 4), 0xdeadbeef);
    CHECK("  rest kept", rd32(back + 12), 4);
    CHECK("  shadow freed", heap_owns(m), 0);
    GL(glBindBuffer, GL_ARRAY_BUFFER, 0);

    // Pixel buffers: glTexSubImage2D from an unpack-buffer offset, glGetTexImage into a pack-buffer offset.
    uint32_t px[2] = {0xdeadbeef, 0x11223344};
    GL(glGenBuffers, 2, ids);
    GL(glBindBuffer, GL_PIXEL_UNPACK_BUFFER, rd32(ids));
    GL(glBufferData, GL_PIXEL_UNPACK_BUFFER, 8, gdup(px, 8), GL_STATIC_DRAW);
    GL(glTexSubImage2D, GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, 4);
    CHECK("glTexSubImage2D pops ret + 36", popped, 40);
    GL(glBindBuffer, GL_PIXEL_UNPACK_BUFFER, 0);
    CHECK("unpack-buffer offset upload", pixel(0, 0), 0x11223344);
    GL(glBindBuffer, GL_PIXEL_PACK_BUFFER, rd32(ids + 4));
    GL(glBufferData, GL_PIXEL_PACK_BUFFER, 16 + 4 * W * H, 0, GL_STREAM_READ);
    GL(glReadPixels, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, 16);
    GL(glGetBufferSubData, GL_PIXEL_PACK_BUFFER, 16, 4, back);
    CHECK("pack-buffer offset readback", rd32(back), 0x11223344);
    GL(glBindBuffer, GL_PIXEL_PACK_BUFFER, 0);

    // Shaders: glShaderSource takes a guest array of guest strings.
    const char *src = "void main() { gl_FragColor = vec4(1.0); }";
    uint32_t strs = heap_calloc(2, 4);
    wr32(strs, guest_strdup("#version 110\n"));
    wr32(strs + 4, guest_strdup(src));
    uint32_t sh = GL(glCreateShader, GL_FRAGMENT_SHADER);
    GL(glShaderSource, sh, 2, strs, 0);
    GL(glCompileShader, sh);
    uint32_t st = heap_calloc(1, 4);
    GL(glGetShaderiv, sh, GL_COMPILE_STATUS, st);
    CHECK("shader compiles", rd32(st), 1);
    uint32_t prog = GL0(glCreateProgram);
    GL(glAttachShader, prog, sh);
    GL(glLinkProgram, prog);
    GL(glGetProgramiv, prog, GL_LINK_STATUS, st);
    CHECK("program links", rd32(st), 1);
    CHECK("glGetUniformLocation: none", GL(glGetUniformLocation, prog, guest_strdup("nope")), 0xffffffff);

    // Sync objects are guest handles.
    uint32_t sync = GL(glFenceSync, GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    CHECK("glFenceSync: small handle", sync >= 1 && sync < 1024, 1);
    CHECK("glIsSync", GL(glIsSync, sync) & 0xff, 1);
    uint32_t w = GL(glClientWaitSync, sync, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000, 0);  // 1 s, 64-bit
    CHECK("glClientWaitSync (u64) pops ret + 16", popped, 20);
    CHECK("  signalled", w == GL_ALREADY_SIGNALED || w == GL_CONDITION_SATISFIED, 1);
    GL(glDeleteSync, sync);
    CHECK("glIsSync after delete", GL(glIsSync, sync) & 0xff, 0);

    CHECK("glGetError", GL0(glGetError), GL_NO_ERROR);

    SDL_GL_DeleteContext(ctx);
    SDL_DestroyWindow(win);
    SDL_Quit();
    printf("%s\n", fails ? "FAIL" : "all ok");
    return fails != 0;
}
