// MSVCR120 memory, string, ctype, conversion and rand functions implemented natively (HLE), in the "C"
// locale. wchar_t is 16 bits on Windows.
#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "heap.h"
#include "host.h"

HOST_CDECL(msvcr120, memcpy) { memcpy(ARG_PTR(0), ARG_PTR(1), ARG(2)); ret_i32(c, ARG(0)); }
HOST_CDECL(msvcr120, memmove) { memmove(ARG_PTR(0), ARG_PTR(1), ARG(2)); ret_i32(c, ARG(0)); }
HOST_CDECL(msvcr120, memset) { memset(ARG_PTR(0), (int)ARG(1), ARG(2)); ret_i32(c, ARG(0)); }
HOST_CDECL(msvcr120, memcmp) { ret_i32(c, memcmp(ARG_PTR(0), ARG_PTR(1), ARG(2))); }
HOST_CDECL(msvcr120, memchr) {
    uint8_t *p = memchr(ARG_PTR(0), (int)ARG(1), ARG(2));
    ret_i32(c, p ? (uint32_t)(p - MEM) : 0);
}
enum { EINVAL_ = 22, ERANGE_ = 34 };
HOST_CDECL(msvcr120, memcpy_s) {  // (dest, destsize, src, count)
    if (ARG(3) > ARG(1)) {
        memset(ARG_PTR(0), 0, ARG(1));
        return ret_i32(c, ERANGE_);
    }
    memcpy(ARG_PTR(0), ARG_PTR(2), ARG(3));
    ret_i32(c, 0);
}

HOST_CDECL(msvcr120, strlen) { ret_i32(c, strlen(ARG_STR(0))); }
HOST_CDECL(msvcr120, strcmp) { ret_i32(c, strcmp(ARG_STR(0), ARG_STR(1))); }
HOST_CDECL(msvcr120, strncmp) { ret_i32(c, strncmp(ARG_STR(0), ARG_STR(1), ARG(2))); }
HOST_CDECL(msvcr120, strcspn) { ret_i32(c, strcspn(ARG_STR(0), ARG_STR(1))); }
HOST_CDECL(msvcr120, strcpy_s) {  // (dest, size, src)
    size_t n = strlen(ARG_STR(2));
    if (!ARG(0) || !ARG(1)) return ret_i32(c, EINVAL_);
    if (n >= ARG(1)) {
        wr8(ARG(0), 0);
        return ret_i32(c, ERANGE_);
    }
    memcpy(ARG_PTR(0), ARG_PTR(2), n + 1);
    ret_i32(c, 0);
}

static uint32_t wlen(uint32_t s) {
    uint32_t n = 0;
    while (rd16(s + 2 * n)) n++;
    return n;
}
HOST_CDECL(msvcr120, wcslen) { ret_i32(c, wlen(ARG(0))); }
HOST_CDECL(msvcr120, wcscpy_s) {  // (dest, size in wchars, src)
    uint32_t n = wlen(ARG(2));
    if (!ARG(0) || !ARG(1)) return ret_i32(c, EINVAL_);
    if (n >= ARG(1)) {
        wr16(ARG(0), 0);
        return ret_i32(c, ERANGE_);
    }
    memmove(ARG_PTR(0), ARG_PTR(2), 2 * (n + 1));
    ret_i32(c, 0);
}
HOST_CDECL(msvcr120, _wcsdup) {
    uint32_t n = 2 * (wlen(ARG(0)) + 1), p = heap_alloc(n);
    if (p) memcpy(P(p), ARG_PTR(0), n);
    ret_i32(c, p);
}

// ctype in the "C" locale: only ASCII has classes. EOF (-1) and bytes >= 0x80 have none.
static int ascii(uint32_t ch) { return ch < 0x80; }
HOST_CDECL(msvcr120, isalnum) { ret_i32(c, ascii(ARG(0)) && isalnum(ARG(0)) ? 0x107 : 0); }
HOST_CDECL(msvcr120, isdigit) { ret_i32(c, ascii(ARG(0)) && isdigit(ARG(0)) ? 0x4 : 0); }
HOST_CDECL(msvcr120, islower) { ret_i32(c, ascii(ARG(0)) && islower(ARG(0)) ? 0x2 : 0); }
HOST_CDECL(msvcr120, isupper) { ret_i32(c, ascii(ARG(0)) && isupper(ARG(0)) ? 0x1 : 0); }
HOST_CDECL(msvcr120, isspace) { ret_i32(c, ascii(ARG(0)) && isspace(ARG(0)) ? 0x8 : 0); }
HOST_CDECL(msvcr120, isxdigit) { ret_i32(c, ascii(ARG(0)) && isxdigit(ARG(0)) ? 0x80 : 0); }
HOST_CDECL(msvcr120, tolower) { ret_i32(c, ascii(ARG(0)) ? (uint32_t)tolower(ARG(0)) : ARG(0)); }
HOST_CDECL(msvcr120, toupper) { ret_i32(c, ascii(ARG(0)) ? (uint32_t)toupper(ARG(0)) : ARG(0)); }

// Numeric conversions. endptr results are translated back to guest addresses.
static void set_end(uint32_t endp, uint32_t s, const char *start, const char *end) {
    if (endp) wr32(endp, s + (uint32_t)(end - start));
}
HOST_CDECL(msvcr120, atoi) { ret_i32(c, (uint32_t)(int32_t)strtol(ARG_STR(0), NULL, 10)); }
HOST_CDECL(msvcr120, strtol) {
    char *end;
    long v = strtol(ARG_STR(0), &end, (int)ARG(2));
    set_end(ARG(1), ARG(0), ARG_STR(0), end);
    ret_i32(c, v > INT32_MAX ? INT32_MAX : v < INT32_MIN ? (uint32_t)INT32_MIN : (uint32_t)v);
}
HOST_CDECL(msvcr120, strtoul) {
    char *end;
    unsigned long v = strtoul(ARG_STR(0), &end, (int)ARG(2));
    set_end(ARG(1), ARG(0), ARG_STR(0), end);
    long sv = (long)v;  // strtoul accepts a sign; keep 32-bit wraparound, clamp out-of-range magnitudes
    ret_i32(c, (sv >= 0 ? v > UINT32_MAX : sv < -(long)UINT32_MAX) ? UINT32_MAX : (uint32_t)v);
}
HOST_CDECL(msvcr120, _strtoui64) {
    char *end;
    unsigned long long v = strtoull(ARG_STR(0), &end, (int)ARG(2));
    set_end(ARG(1), ARG(0), ARG_STR(0), end);
    ret_i64(c, v);
}
HOST_CDECL(msvcr120, strtod) {
    char *end;
    double v = strtod(ARG_STR(0), &end);
    set_end(ARG(1), ARG(0), ARG_STR(0), end);
    ret_f64(c, v);
}

// rand: the CRT's per-thread LCG, so sequences match Windows for a given seed.
static _Thread_local uint32_t HOLDRAND = 1;
HOST_CDECL(msvcr120, srand) { HOLDRAND = ARG(0); }
HOST_CDECL(msvcr120, rand) {
    HOLDRAND = HOLDRAND * 214013u + 2531011u;
    ret_i32(c, (HOLDRAND >> 16) & 0x7fff);
}
HOST_CDECL(msvcr120, rand_s) {  // (unsigned *out)
    arc4random_buf(ARG_PTR(0), 4);
    ret_i32(c, 0);
}

// Locale: only the "C" locale exists. setlocale accepts "C" (and "" or a query) and fails for others.
static uint32_t gstr(const char *s) {  // a permanent guest copy of a host string
    uint32_t p = heap_alloc(strlen(s) + 1);
    strcpy((char *)P(p), s);
    return p;
}
static uint32_t gwstr(const char *s) {  // ... widened to UTF-16 (ASCII only)
    size_t n = strlen(s);
    uint32_t p = heap_alloc(2 * (n + 1));
    for (size_t i = 0; i <= n; i++) wr16(p + 2 * i, (uint8_t)s[i]);
    return p;
}
static uint32_t C_NAME, PCTYPE, LOCALE_NAMES, LCONV;
static void locale_tables(void) {
    if (PCTYPE) return;
    C_NAME = gstr("C");
    uint32_t t = heap_calloc(257, 2);  // _ctype: entry 0 is EOF, _pctype points at entry 1
    for (int ch = 0; ch < 128; ch++) {
        uint16_t f = (isupper(ch) ? 0x1 : 0) | (islower(ch) ? 0x2 : 0) | (isdigit(ch) ? 0x4 : 0) |
                     (isspace(ch) ? 0x8 : 0) | (ispunct(ch) ? 0x10 : 0) | (iscntrl(ch) ? 0x20 : 0) |
                     (ch == ' ' ? 0x40 : 0) | (isxdigit(ch) ? 0x80 : 0) | (isalpha(ch) ? 0x100 : 0);
        wr16(t + 2 + 2 * ch, f);
    }
    PCTYPE = t + 2;
    LOCALE_NAMES = heap_calloc(6, 4);  // LC_ALL..LC_TIME locale names: NULL means "C"
    // struct lconv (32-bit VC12): 10 char *, 8 char, 10 wchar_t *.
    LCONV = heap_calloc(1, 88);
    uint32_t empty = gstr(""), wempty = gwstr("");
    for (int i = 0; i < 10; i++) wr32(LCONV + 4 * i, empty);
    wr32(LCONV, gstr("."));
    for (int i = 0; i < 8; i++) wr8(LCONV + 40 + i, 127);  // CHAR_MAX: not available
    for (int i = 0; i < 10; i++) wr32(LCONV + 48 + 4 * i, wempty);
    wr32(LCONV + 48, gwstr("."));
}
static pthread_once_t LOCALE_ONCE = PTHREAD_ONCE_INIT;
#define LOCALE() pthread_once(&LOCALE_ONCE, locale_tables)

HOST_CDECL(msvcr120, setlocale) {  // (category, locale)
    LOCALE();
    const char *l = ARG_STR(1);
    ret_i32(c, !l || !*l || !strcmp(l, "C") ? C_NAME : 0);
}
HOST_CDECL(msvcr120, localeconv) { LOCALE(); ret_i32(c, LCONV); }
HOST_CDECL(msvcr120, __pctype_func) { LOCALE(); ret_i32(c, PCTYPE); }
HOST_CDECL(msvcr120, ___lc_locale_name_func) { LOCALE(); ret_i32(c, LOCALE_NAMES); }
HOST_CDECL(msvcr120, ___lc_codepage_func) { ret_i32(c, 0); }
HOST_CDECL(msvcr120, ___lc_collate_cp_func) { ret_i32(c, 0); }
HOST_CDECL(msvcr120, ___mb_cur_max_func) { ret_i32(c, 1); }
