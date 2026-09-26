// MSVCR120 stdio HLE (msvcr120_stdio.c) pieces other files use.
#pragma once
#include <stddef.h>
#include <stdint.h>

// Format like MSVC's printf, with the arguments in guest memory from guest address `va`. Returns a
// malloc'd NUL-terminated string and its length in *len.
char *crt_vformat(const char *fmt, uint32_t va, size_t *len);
