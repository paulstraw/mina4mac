// SDL2 imports implemented natively (HLE): the C-library-style helpers SDL2main's WinMain (0xdfb400,
// statically linked into noita.exe) uses to build SDL_main's argv, and SDL_SetMainReady. These never touch
// host SDL: SDL_malloc/SDL_free are the guest heap, and SDL wchar_t is Windows' 16-bit WCHAR.
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "heap.h"
#include "hle.h"
#include "host.h"

enum { SDL_MAIN = 0x80b6f0 };

HOST_CDECL(SDL2, SDL_malloc) { ret_i32(c, heap_alloc(ARG(0) ? ARG(0) : 1)); }  // SDL_malloc(0) is non-NULL
HOST_CDECL(SDL2, SDL_free) { heap_free(ARG(0)); }

HOST_CDECL(SDL2, SDL_isspace) {
    uint32_t x = ARG(0);
    ret_i32(c, x == ' ' || x == '\t' || x == '\n' || x == '\f' || x == '\r' || x == '\v');
}

HOST_CDECL(SDL2, SDL_wcslen) {
    uint32_t n = 0;
    while (rd16(ARG(0) + 2 * n)) n++;
    ret_i32(c, n);
}

// SDL_iconv_string(tocode, fromcode, inbuf, inbytesleft) returns an SDL_malloc'd copy of the input in the
// target encoding. Only what WinMain needs: UTF-16LE (and UCS-2) to UTF-8, stopping at the input's NUL,
// which WinMain includes in inbytesleft. Anything else fails with NULL, as SDL does for unknown encodings.
HOST_CDECL(SDL2, SDL_iconv_string) {
    const char *to = ARG_STR(0), *from = ARG_STR(1);
    char buf[65536];
    if (strcasecmp(to, "UTF-8") || (strcasecmp(from, "UTF-16LE") && strcasecmp(from, "UCS-2-INTERNAL"))) {
        fprintf(stderr, "SDL_iconv_string: %s -> %s not supported\n", from, to);
        return ret_i32(c, 0);
    }
    if (!utf16_to_utf8(ARG(2), buf, sizeof buf)) return ret_i32(c, 0);
    ret_i32(c, guest_strdup(buf));
}

// WinMain calls this just before SDL_main(argc, argv) (0xdfb524), so it marks SDL_main's entry in the
// trace. argc and argv are in ebx and edi there (the lifted code syncs registers to c around calls).
HOST_CDECL(SDL2, SDL_SetMainReady) {
    if (!rt_trace) return;
    fprintf(stderr, "[mina4mac] entering SDL_main %#x (argc %u", SDL_MAIN, c->ebx);
    for (uint32_t i = 0; i < c->ebx; i++) fprintf(stderr, ", \"%s\"", (char *)P(rd32(c->edi + 4 * i)));
    fprintf(stderr, ")\n");
}
