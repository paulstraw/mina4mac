// Helpers shared by the HLE (natively implemented) DLL files.
#pragma once
#include <stddef.h>
#include "cpu.h"

// A guest heap copy of a host string (0 if the heap is exhausted).
uint32_t guest_strdup(const char *s);

// Windows paths. The host file system appears as drive Z: (as in Wine), so the guest's working directory
// (the game directory) is e.g. "Z:\Users\me\noitamac\build\game". host_path translates a Windows
// path (relative, or on Z:) to a host path: the drive is dropped and backslashes become slashes. Returns 0
// if it doesn't fit in n bytes; NOITAMAC_TRACE_FILES=1 logs each path to stderr. win_path does the reverse for an absolute host path.
int host_path(const char *win, char *out, size_t n);
int win_path(const char *host, char *out, size_t n);

// The calling guest thread's CRT errno (msvcr120 _errno), set from a host errno value.
void crt_set_errno(CPU *c, int host_errno);

// Convert the NUL-terminated UTF-16 string at guest `ws` to UTF-8 (unpaired surrogates become U+FFFD).
// Returns 0 if it doesn't fit in n bytes.
int utf16_to_utf8(uint32_t ws, char *out, size_t n);

// Convert UTF-8 to UTF-16 in guest memory at `out`, room for `cap` units including the NUL. Returns the
// length in units without the NUL, or if it doesn't fit (nothing written) the needed size including it.
uint32_t utf8_to_utf16(const char *s, uint32_t out, uint32_t cap);
