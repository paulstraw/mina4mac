// The OpenGL bridge (opengl32.dll): generated __stdcall thunks (build/gen_all/gl_gen.c, tools/gen_gl.py) plus
// the hand-written ones and helpers in opengl32.c. The host side is macOS's legacy 2.1 context.
#pragma once
#define GL_SILENCE_DEPRECATION
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#define GL_GLEXT_PROTOTYPES
#include <OpenGL/glext.h>

#include "hle.h"
#include "host.h"

// A context is current on this thread. Without one, GL calls do nothing and return 0, as on Windows.
#define GL_CTX __builtin_expect(CGLGetCurrentContext() != NULL, 1)

// Pointer argument `p` of a call whose data comes from (or goes to) the buffer bound at `binding`
// (GL_ARRAY_BUFFER_BINDING, ...): a buffer offset while one is bound, otherwise a guest pointer.
void *gl_buf_ptr(GLenum binding, uint32_t p);

// A pointer GL hands back (glGetPointerv): the guest address if it points into guest memory, otherwise
// the buffer offset it holds.
uint32_t gl_guest_ptr(const void *p);

// GLsync objects as 32-bit guest handles (0 for NULL).
uint32_t gl_sync_guest(GLsync s);
GLsync gl_sync_host(uint32_t h);

// GetProcAddress on opengl32.dll: the thunk of a GL function the bridge implements, otherwise 0 (what a
// driver returns for functions it doesn't have). Names that don't start with "gl" (wgl*) always get a thunk.
uint32_t gl_proc_address(const char *name);
