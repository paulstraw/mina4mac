// The SDL2 bridge's runtime half (sdl2.c), used by the thunks tools/gen_sdl.py generates (sdl2_gen.c).
//
// Host SDL objects appear to the guest as 32-bit handles: the guest address of a guest heap block whose
// 16-byte header holds the host pointer. Opaque objects (windows, joysticks, RWops, GL contexts, ...)
// get an empty block; a host pointer maps to the same handle for as long as it lives, and handles are
// never freed (a later object at the same host address reuses it). Surfaces get a block holding a guest
// SDL_Surface (with SDL_PixelFormat and SDL_Palette mirrors) that is re-synced from the host surface after
// every call that takes it, and their pixels always live in guest memory, so the game can write them.
#pragma once
#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "host.h"
#include "sdl2_layout.h"

uint32_t sdl_handle(void *h);         // handle for a host object (NULL: 0), created on first use
void *sdl_host(uint32_t g);           // host object of a handle (0: NULL); exits on anything else
void *sdl_host_or_null(uint32_t g);   // same, but NULL for non-handles (garbage in guest out-structs)

// Copy s (NULL: return 0) into the guest heap buffer *buf, growing it as needed; returns *buf.
uint32_t sdl_guest_str(uint32_t *buf, const char *s);

// The guest mirror of host surface s (NULL: 0), created on first use. A surface whose pixels are in
// host memory (created by SDL itself) is first replaced by an equivalent one with pixels in the guest
// heap (freed with the surface).
uint32_t sdl_surface_guest(SDL_Surface *s);
SDL_Surface *sdl_surface_host(uint32_t g);  // exits if g isn't a surface mirror (0: NULL)
void sdl_surface_sync(SDL_Surface *s);      // refresh s's mirror (and its format/palette) from s
