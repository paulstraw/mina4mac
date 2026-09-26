// opengl32 implemented natively (HLE): a stand-in until the generated GL bridge exists. GL entry points
// are __stdcall on Win32. Each one here forwards to the host's OpenGL if a context is current and is a
// no-op otherwise, as opengl32 is on Windows (noita.exe makes GL calls before it creates its window).
#define GL_SILENCE_DEPRECATION
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#define GL_GLEXT_PROTOTYPES
#include <OpenGL/glext.h>

#include "hle.h"
#include "host.h"

#define GL(name, argbytes, ...) \
    HOST_STDCALL(opengl32, name, argbytes) { if (CGLGetCurrentContext()) name(__VA_ARGS__); }
// The same for functions with a result, which is 0 without a context.
#define GLR(name, argbytes, ...) \
    HOST_STDCALL(opengl32, name, argbytes) { ret_i32(c, CGLGetCurrentContext() ? (uint32_t)name(__VA_ARGS__) : 0); }

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
GL(glBlendFuncSeparate, 16, ARG(0), ARG(1), ARG(2), ARG(3))
GL(glBlendEquation, 4, ARG(0))
GL(glScissor, 16, ARG(0), ARG(1), ARG(2), ARG(3))
GL(glColorMask, 16, ARG(0), ARG(1), ARG(2), ARG(3))
GL(glDepthMask, 4, ARG(0))
GL(glLineWidth, 4, ARG_F32(0))
GL(glPointSize, 4, ARG_F32(0))
GL(glPushMatrix, 0)
GL(glPopMatrix, 0)
GL(glLoadMatrixf, 4, ARG_PTR(0))
GL(glMultMatrixf, 4, ARG_PTR(0))
GL(glGetIntegerv, 8, ARG(0), ARG_PTR(1))
GL(glGetFloatv, 8, ARG(0), ARG_PTR(1))
GLR(glGetError, 0)
GL(glFlush, 0)
GL(glFinish, 0)
GL(glReadPixels, 28, ARG(0), ARG(1), ARG(2), ARG(3), ARG(4), ARG(5), ARG_PTR(6))
GL(glTexParameterf, 12, ARG(0), ARG(1), ARG_F32(2))
GL(glGenerateMipmap, 4, ARG(0))
GL(glCopyTexSubImage2D, 32, ARG(0), ARG(1), ARG(2), ARG(3), ARG(4), ARG(5), ARG(6), ARG(7))
GL(glPixelStorei, 8, ARG(0), ARG(1))
GL(glGenTextures, 8, ARG(0), ARG_PTR(1))
GL(glDeleteTextures, 8, ARG(0), ARG_PTR(1))
GL(glBindTexture, 8, ARG(0), ARG(1))
GL(glTexParameteri, 12, ARG(0), ARG(1), ARG(2))
GL(glTexImage2D, 36, ARG(0), ARG(1), ARG(2), ARG(3), ARG(4), ARG(5), ARG(6), ARG(7), ARG_PTR(8))
GL(glTexSubImage2D, 36, ARG(0), ARG(1), ARG(2), ARG(3), ARG(4), ARG(5), ARG(6), ARG(7), ARG_PTR(8))

// GL 2.0 shaders and programs (the legacy 2.1 context has them).
GLR(glCreateShader, 4, ARG(0))
GLR(glCreateProgram, 0)
GL(glCompileShader, 4, ARG(0))
GL(glDeleteShader, 4, ARG(0))
GL(glDeleteProgram, 4, ARG(0))
GL(glAttachShader, 8, ARG(0), ARG(1))
GL(glDetachShader, 8, ARG(0), ARG(1))
GL(glLinkProgram, 4, ARG(0))
GL(glValidateProgram, 4, ARG(0))
GL(glUseProgram, 4, ARG(0))
GL(glGetShaderiv, 12, ARG(0), ARG(1), ARG_PTR(2))
GL(glGetProgramiv, 12, ARG(0), ARG(1), ARG_PTR(2))
GL(glGetShaderInfoLog, 16, ARG(0), ARG(1), ARG_PTR(2), ARG_PTR(3))
GL(glGetProgramInfoLog, 16, ARG(0), ARG(1), ARG_PTR(2), ARG_PTR(3))
GL(glBindAttribLocation, 12, ARG(0), ARG(1), ARG_STR(2))
GLR(glGetUniformLocation, 8, ARG(0), ARG_STR(1))
GLR(glGetAttribLocation, 8, ARG(0), ARG_STR(1))
GL(glUniform1i, 8, ARG(0), ARG(1))
GL(glUniform1f, 8, ARG(0), ARG_F32(1))
GL(glUniform2f, 12, ARG(0), ARG_F32(1), ARG_F32(2))
GL(glUniform3f, 16, ARG(0), ARG_F32(1), ARG_F32(2), ARG_F32(3))
GL(glUniform4f, 20, ARG(0), ARG_F32(1), ARG_F32(2), ARG_F32(3), ARG_F32(4))
GL(glUniform1fv, 12, ARG(0), ARG(1), ARG_PTR(2))
GL(glUniform2fv, 12, ARG(0), ARG(1), ARG_PTR(2))
GL(glUniform3fv, 12, ARG(0), ARG(1), ARG_PTR(2))
GL(glUniform4fv, 12, ARG(0), ARG(1), ARG_PTR(2))
GL(glUniformMatrix4fv, 16, ARG(0), ARG(1), ARG(2), ARG_PTR(3))

// ARB_framebuffer_object (part of the legacy context's extensions).
GL(glGenFramebuffers, 8, ARG(0), ARG_PTR(1))
GL(glDeleteFramebuffers, 8, ARG(0), ARG_PTR(1))
GL(glBindFramebuffer, 8, ARG(0), ARG(1))
GL(glFramebufferTexture2D, 20, ARG(0), ARG(1), ARG(2), ARG(3), ARG(4))
GLR(glCheckFramebufferStatus, 4, ARG(0))
GL(glGenRenderbuffers, 8, ARG(0), ARG_PTR(1))
GL(glDeleteRenderbuffers, 8, ARG(0), ARG_PTR(1))
GL(glBindRenderbuffer, 8, ARG(0), ARG(1))
GL(glRenderbufferStorage, 16, ARG(0), ARG(1), ARG(2), ARG(3))
GL(glFramebufferRenderbuffer, 16, ARG(0), ARG(1), ARG(2), ARG(3))

// Buffer objects (glMapBuffer would need a guest shadow copy; the GL bridge will do that).
GL(glGenBuffers, 8, ARG(0), ARG_PTR(1))
GL(glDeleteBuffers, 8, ARG(0), ARG_PTR(1))
GL(glBindBuffer, 8, ARG(0), ARG(1))
GL(glBufferData, 16, ARG(0), (GLsizeiptr)(int32_t)ARG(1), ARG_PTR(2), ARG(3))
GL(glBufferSubData, 16, ARG(0), (GLintptr)(int32_t)ARG(1), (GLsizeiptr)(int32_t)ARG(2), ARG_PTR(3))

// Vertex arrays. With a buffer bound, array pointers and element indices are offsets into it, not addresses.
static const void *array_ptr(GLenum binding, uint32_t p) {
    GLint buf = 0;
    glGetIntegerv(binding, &buf);
    return buf ? (const void *)(uintptr_t)p : host_ptr(p);
}
#define VPTR(n) array_ptr(GL_ARRAY_BUFFER_BINDING, ARG(n))
GL(glEnableClientState, 4, ARG(0))
GL(glDisableClientState, 4, ARG(0))
GL(glVertexPointer, 16, ARG(0), ARG(1), ARG(2), VPTR(3))
GL(glTexCoordPointer, 16, ARG(0), ARG(1), ARG(2), VPTR(3))
GL(glColorPointer, 16, ARG(0), ARG(1), ARG(2), VPTR(3))
GL(glEnableVertexAttribArray, 4, ARG(0))
GL(glDisableVertexAttribArray, 4, ARG(0))
GL(glVertexAttribPointer, 24, ARG(0), ARG(1), ARG(2), ARG(3), ARG(4), VPTR(5))
GL(glDrawArrays, 12, ARG(0), ARG(1), ARG(2))
GL(glDrawElements, 16, ARG(0), ARG(1), ARG(2), array_ptr(GL_ELEMENT_ARRAY_BUFFER_BINDING, ARG(3)))
GL(glActiveTexture, 4, ARG(0))
GL(glColor4f, 16, ARG_F32(0), ARG_F32(1), ARG_F32(2), ARG_F32(3))

// glShaderSource(shader, count, strings, lengths): `strings` is a guest array of guest string pointers.
HOST_STDCALL(opengl32, glShaderSource, 16) {
    if (!CGLGetCurrentContext()) return;
    uint32_t n = ARG(1), strs = ARG(2);
    const GLchar *v[64];
    if (n > 64) n = 64;  // not expected: one shader is a handful of strings
    for (uint32_t i = 0; i < n; i++) v[i] = host_ptr(rd32(strs + 4 * i));
    glShaderSource(ARG(0), n, v, ARG_PTR(3));
}

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
