// opengl32 implemented natively (HLE): a stand-in until the generated GL bridge exists. GL entry points
// are __stdcall on Win32. Each one here forwards to the host's OpenGL if a context is current and is a
// no-op otherwise, as opengl32 is on Windows (noita.exe makes GL calls before it creates its window).
#define GL_SILENCE_DEPRECATION
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>

#include "hle.h"
#include "host.h"

#define GL(name, argbytes, ...) \
    HOST_STDCALL(opengl32, name, argbytes) { if (CGLGetCurrentContext()) name(__VA_ARGS__); }

GL(glMatrixMode, 4, ARG(0))
GL(glLoadIdentity, 0)
GL(glOrtho, 48, ARG_F64(0), ARG_F64(2), ARG_F64(4), ARG_F64(6), ARG_F64(8), ARG_F64(10))
GL(glViewport, 16, ARG(0), ARG(1), ARG(2), ARG(3))
GL(glClearColor, 16, ARG_F32(0), ARG_F32(1), ARG_F32(2), ARG_F32(3))
GL(glClear, 4, ARG(0))
GL(glScalef, 12, ARG_F32(0), ARG_F32(1), ARG_F32(2))
GL(glTranslatef, 12, ARG_F32(0), ARG_F32(1), ARG_F32(2))
GL(glEnable, 4, ARG(0))
GL(glDisable, 4, ARG(0))
GL(glBlendFunc, 8, ARG(0), ARG(1))
GL(glPixelStorei, 8, ARG(0), ARG(1))

// glGetString: the host's string, copied into guest memory once per name (NULL without a context).
HOST_STDCALL(opengl32, glGetString, 4) {
    static uint32_t cache[8];
    uint32_t name = ARG(0), i = name - GL_VENDOR;  // GL_VENDOR, _RENDERER, _VERSION, _EXTENSIONS: 0x1f00..3
    const char *s = CGLGetCurrentContext() ? (const char *)glGetString(name) : NULL;
    if (!s) return ret_i32(c, 0);
    if (i >= 4) return ret_i32(c, guest_strdup(s));  // not expected; leaks
    if (!cache[i]) cache[i] = guest_strdup(s);
    ret_i32(c, cache[i]);
}
