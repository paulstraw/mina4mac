// opengl32 implemented natively (HLE). GL entry points are __stdcall on Win32. Most are generated
// (tools/gen_gl.py → build/gen_all/gl_gen.c); this file has the helpers they use and the functions whose
// arguments need more than per-argument marshalling.
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "heap.h"
#include "opengl32.h"

void *gl_buf_ptr(GLenum binding, uint32_t p) {
    GLint buf = 0;
    glGetIntegerv(binding, &buf);
    return buf ? (void *)(uintptr_t)p : host_ptr(p);
}

uint32_t gl_guest_ptr(const void *p) {
    uintptr_t a = (uintptr_t)p;
    return a >= (uintptr_t)MEM && a - (uintptr_t)MEM < (1ull << 32) ? (uint32_t)(a - (uintptr_t)MEM) : (uint32_t)a;
}

uint32_t gl_proc_address(const char *name) {
    if (strncmp(name, "gl", 2) || rt_has_import("opengl32.dll", name)) return rt_thunk("opengl32.dll", name);
    return 0;
}

// GLsync handles: slot index + 1 in a fixed table (the game makes few fences at a time).
enum { MAX_SYNCS = 1024 };
static GLsync SYNCS[MAX_SYNCS];
static pthread_mutex_t SYNCS_LOCK = PTHREAD_MUTEX_INITIALIZER;

uint32_t gl_sync_guest(GLsync s) {
    if (!s) return 0;
    pthread_mutex_lock(&SYNCS_LOCK);
    uint32_t i = 0;
    while (i < MAX_SYNCS && SYNCS[i]) i++;
    if (i < MAX_SYNCS) SYNCS[i] = s;
    pthread_mutex_unlock(&SYNCS_LOCK);
    if (i == MAX_SYNCS) { fprintf(stderr, "opengl32: too many GLsync objects\n"); exit(2); }
    return i + 1;
}

GLsync gl_sync_host(uint32_t h) { return h - 1 < MAX_SYNCS ? SYNCS[h - 1] : NULL; }

HOST_STDCALL(opengl32, glDeleteSync, 4) {
    uint32_t h = ARG(0);
    if (!GL_CTX || h - 1 >= MAX_SYNCS) return;
    glDeleteSync(SYNCS[h - 1]);
    __atomic_store_n(&SYNCS[h - 1], NULL, __ATOMIC_RELEASE);
}

// glShaderSource(shader, count, strings, lengths): `strings` is a guest array of guest string pointers.
HOST_STDCALL(opengl32, glShaderSource, 16) {
    if (!GL_CTX) return;
    uint32_t n = ARG(1), strs = ARG(2);
    const GLchar *v[64];
    if (n > 64) n = 64;  // not expected: one shader is a handful of strings
    for (uint32_t i = 0; i < n; i++) v[i] = host_ptr(rd32(strs + 4 * i));
    glShaderSource(ARG(0), n, v, ARG_PTR(3));
}

// glMultiDrawElements[BaseVertex](mode, counts, type, indices, drawcount[, basevertex]): `indices` is a guest
// array of guest pointers, or of offsets into the bound element buffer.
static void multi_draw(uint32_t argp, int base_vertex) {
    GLsizei n = (GLsizei)ARG(4);
    const void *idx[256];
    if (n > 256) n = 256;  // not expected
    for (GLsizei i = 0; i < n; i++) idx[i] = gl_buf_ptr(GL_ELEMENT_ARRAY_BUFFER_BINDING, rd32(ARG(3) + 4 * i));
    if (base_vertex) glMultiDrawElementsBaseVertex(ARG(0), ARG_PTR(1), ARG(2), idx, n, ARG_PTR(5));
    else glMultiDrawElements(ARG(0), ARG_PTR(1), ARG(2), idx, n);
}
HOST_STDCALL(opengl32, glMultiDrawElements, 20) { if (GL_CTX) multi_draw(argp, 0); }
HOST_STDCALL(opengl32, glMultiDrawElementsBaseVertex, 24) { if (GL_CTX) multi_draw(argp, 1); }

// glGetString: the host's string, copied into guest memory once per name (NULL without a context).
HOST_STDCALL(opengl32, glGetString, 4) {
    static uint32_t cache[8];
    uint32_t name = ARG(0), i = name - GL_VENDOR;  // GL_VENDOR, _RENDERER, _VERSION, _EXTENSIONS: 0x1f00..3
    const char *s = GL_CTX ? (const char *)glGetString(name) : NULL;
    if (!s) return ret_i32(c, 0);
    if (i >= 4) {
        if (name != GL_SHADING_LANGUAGE_VERSION) return ret_i32(c, guest_strdup(s));  // not expected; leaks
        i = 4;
    }
    if (!__atomic_load_n(&cache[i], __ATOMIC_ACQUIRE)) {
        uint32_t g = guest_strdup(s), zero = 0;
        if (!__atomic_compare_exchange_n(&cache[i], &zero, g, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) heap_free(g);
    }
    ret_i32(c, cache[i]);
}

// Buffer mapping. The host's mapping is a host pointer, so the guest gets a shadow copy in the guest heap:
// filled from the mapping unless the access is write-only, and copied back on unmap unless it's read-only.
// Keyed by context and buffer name.
enum { MAX_MAPS = 64 };
static struct Map { CGLContextObj ctx; GLuint buf; GLenum access; void *host; uint32_t guest, size; } MAPS[MAX_MAPS];
static pthread_mutex_t MAPS_LOCK = PTHREAD_MUTEX_INITIALIZER;

static GLenum binding_of(GLenum target) {
    switch (target) {
    case GL_ARRAY_BUFFER: return GL_ARRAY_BUFFER_BINDING;
    case GL_ELEMENT_ARRAY_BUFFER: return GL_ELEMENT_ARRAY_BUFFER_BINDING;
    case GL_PIXEL_PACK_BUFFER: return GL_PIXEL_PACK_BUFFER_BINDING;
    case GL_PIXEL_UNPACK_BUFFER: return GL_PIXEL_UNPACK_BUFFER_BINDING;
    default: return 0;
    }
}

// The map of the buffer bound to `target` in the current context, or with `create` a free slot for it.
static struct Map *find_map(GLenum target, int create) {
    GLint buf = 0;
    GLenum b = binding_of(target);
    if (b) glGetIntegerv(b, &buf);
    CGLContextObj ctx = CGLGetCurrentContext();
    struct Map *free = NULL;
    for (int i = 0; i < MAX_MAPS; i++) {
        if (MAPS[i].ctx == ctx && MAPS[i].buf == (GLuint)buf) return &MAPS[i];
        if (!MAPS[i].ctx && !free) free = &MAPS[i];
    }
    if (!create) return NULL;
    if (!free) { fprintf(stderr, "opengl32: too many mapped buffers\n"); exit(2); }
    *free = (struct Map){.ctx = ctx, .buf = (GLuint)buf};
    return free;
}

HOST_STDCALL(opengl32, glMapBuffer, 8) {  // (target, access)
    GLenum target = ARG(0), access = ARG(1);
    void *host = GL_CTX ? glMapBuffer(target, access) : NULL;
    if (!host) return ret_i32(c, 0);
    GLint size = 0;
    glGetBufferParameteriv(target, GL_BUFFER_SIZE, &size);
    uint32_t g = heap_alloc((uint32_t)size);
    if (!g) { glUnmapBuffer(target); return ret_i32(c, 0); }
    if (access != GL_WRITE_ONLY) memcpy(P(g), host, (size_t)size);
    pthread_mutex_lock(&MAPS_LOCK);
    struct Map *m = find_map(target, 1);
    m->access = access, m->host = host, m->guest = g, m->size = (uint32_t)size;
    pthread_mutex_unlock(&MAPS_LOCK);
    ret_i32(c, g);
}

HOST_STDCALL(opengl32, glUnmapBuffer, 4) {  // (target)
    GLenum target = ARG(0);
    if (!GL_CTX) return ret_i32(c, 0);
    pthread_mutex_lock(&MAPS_LOCK);
    struct Map *m = find_map(target, 0), copy = m ? *m : (struct Map){0};
    if (m) *m = (struct Map){0};
    pthread_mutex_unlock(&MAPS_LOCK);
    if (copy.host && copy.access != GL_READ_ONLY) memcpy(copy.host, P(copy.guest), copy.size);
    heap_free(copy.guest);
    ret_i32(c, glUnmapBuffer(target));
}

HOST_STDCALL(opengl32, glGetBufferPointerv, 12) {  // (target, pname, params): GL_BUFFER_MAP_POINTER is the shadow
    if (!GL_CTX || !ARG(2)) return;
    pthread_mutex_lock(&MAPS_LOCK);
    struct Map *m = find_map(ARG(0), 0);
    wr32(ARG(2), m ? m->guest : 0);
    pthread_mutex_unlock(&MAPS_LOCK);
}

// wglGetProcAddress(name): the game's loader asks here for names GetProcAddress returned NULL for. Same answer
// (so still NULL for GL functions the bridge doesn't have), and NULL for wgl extensions.
HOST_STDCALL(opengl32, wglGetProcAddress, 4) {
    const char *name = ARG_STR(0);
    ret_i32(c, name && !strncmp(name, "gl", 2) ? gl_proc_address(name) : 0);
}
