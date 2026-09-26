// Helpers shared by the HLE files (hle.h).
#include "hle.h"

#include <errno.h>
#include <string.h>

#include "heap.h"
#include "proc.h"

uint32_t guest_strdup(const char *s) {
    uint32_t p = heap_alloc(strlen(s) + 1);
    if (p) strcpy((char *)P(p), s);
    return p;
}

int host_path(const char *win, char *out, size_t n) {
    if ((win[0] == 'Z' || win[0] == 'z') && win[1] == ':') win += 2;
    size_t len = strlen(win);
    if (len >= n) return 0;
    for (size_t i = 0; i <= len; i++) out[i] = win[i] == '\\' ? '/' : win[i];
    return 1;
}

int win_path(const char *host, char *out, size_t n) {
    size_t len = strlen(host);
    if (len + 3 > n) return 0;
    out[0] = 'Z', out[1] = ':';
    for (size_t i = 0; i <= len; i++) out[i + 2] = host[i] == '/' ? '\\' : host[i];
    return 1;
}

void crt_set_errno(CPU *c, int e) {
    // Windows' errno values match the host's up to ERANGE (34), except EAGAIN (11 there, 35 here).
    wr32(c->fs_base + TEB_CRT_ERRNO, e == EAGAIN ? 11 : e <= 34 ? e : 22);
}

int utf16_to_utf8(uint32_t ws, char *out, size_t n) {
    size_t o = 0;
    for (uint32_t u; (u = rd16(ws)); ws += 2) {
        if (u >= 0xd800 && u < 0xdc00 && rd16(ws + 2) >= 0xdc00 && rd16(ws + 2) < 0xe000) {
            u = 0x10000 + ((u - 0xd800) << 10) + (rd16(ws + 2) - 0xdc00);
            ws += 2;
        } else if (u >= 0xd800 && u < 0xe000) u = 0xfffd;
        char b[4];
        int k = u < 0x80 ? (b[0] = u, 1)
              : u < 0x800 ? (b[0] = 0xc0 | u >> 6, b[1] = 0x80 | (u & 0x3f), 2)
              : u < 0x10000 ? (b[0] = 0xe0 | u >> 12, b[1] = 0x80 | ((u >> 6) & 0x3f), b[2] = 0x80 | (u & 0x3f), 3)
              : (b[0] = 0xf0 | u >> 18, b[1] = 0x80 | ((u >> 12) & 0x3f), b[2] = 0x80 | ((u >> 6) & 0x3f),
                 b[3] = 0x80 | (u & 0x3f), 4);
        if (o + k >= n) return 0;
        memcpy(out + o, b, k);
        o += k;
    }
    out[o] = 0;
    return 1;
}

uint32_t utf8_to_utf16(const char *s, uint32_t out, uint32_t cap) {
    uint16_t buf[8192];
    uint32_t n = 0;
    for (const uint8_t *p = (const uint8_t *)s; *p && n + 2 < sizeof buf / 2;) {
        uint32_t u = *p++, extra = u >= 0xf0 ? 3 : u >= 0xe0 ? 2 : u >= 0xc0 ? 1 : 0;
        if (extra) u &= 0x3f >> extra;
        for (; extra && (*p & 0xc0) == 0x80; extra--) u = u << 6 | (*p++ & 0x3f);
        if (extra) u = 0xfffd;  // truncated sequence
        if (u >= 0x10000) buf[n++] = 0xd800 + ((u - 0x10000) >> 10), buf[n++] = 0xdc00 + (u & 0x3ff);
        else buf[n++] = u;
    }
    if (n + 1 > cap) return n + 1;
    for (uint32_t i = 0; i < n; i++) wr16(out + 2 * i, buf[i]);
    wr16(out + 2 * n, 0);
    return n;
}
