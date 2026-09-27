// MSVCR120 stdio implemented natively (HLE). A guest FILE is the CRT's 32-byte struct in guest memory
// (_iob[0..2] are stdin/stdout/stderr); its _file field indexes FILES, the host FILE * behind it. _cnt
// stays 0 so any inlined getc/putc macro would fall through to _filbuf/_flsbuf.
//
// Text mode does no CR/LF translation: files are read and written as they are on disk.
#include "msvcr120_stdio.h"

#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "heap.h"
#include "hle.h"
#include "host.h"

enum { FILE_SIZE = 32, F_FLAG = 12, F_FILE = 16, NIOB = 20, MAX_FILES = 1024 };
enum { IOREAD = 0x1, IOWRT = 0x2, IORW = 0x80 };
static struct { FILE *f; int binary; } FILES[MAX_FILES];
static uint32_t IOB;  // guest _iob[NIOB]
static pthread_mutex_t FILES_LOCK = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t IOB_ONCE = PTHREAD_ONCE_INIT;

static void iob_init(void) {
    IOB = heap_calloc(NIOB, FILE_SIZE);
    FILES[0].f = stdin, FILES[1].f = stdout, FILES[2].f = stderr;
    static const uint32_t flags[] = {IOREAD, IOWRT, IOWRT};
    for (int i = 0; i < 3; i++) {
        wr32(IOB + FILE_SIZE * i + F_FLAG, flags[i]);
        wr32(IOB + FILE_SIZE * i + F_FILE, i);
    }
}

HOST_CDECL(msvcr120, __iob_func) {
    pthread_once(&IOB_ONCE, iob_init);
    ret_i32(c, IOB);
}

static FILE *hf(uint32_t gf) {  // host FILE of a guest FILE
    uint32_t i = gf ? rd32(gf + F_FILE) : MAX_FILES;
    if (i >= MAX_FILES || !FILES[i].f) { fprintf(stderr, "guest FILE %#x is not open\n", gf); exit(6); }
    return FILES[i].f;
}

// Open: host mode from the CRT's ("r"/"w"/"a", "+", "b"/"t", and flags the host lacks, which are dropped).
static uint32_t crt_fopen(CPU *c, const char *win, const char *mode) {
    pthread_once(&IOB_ONCE, iob_init);
    char path[4096], hmode[8];
    int n = 0, binary = 0;
    for (const char *m = mode; *m && *m != ',' && n < 6; m++) {
        if (*m == 'b') binary = 1;
        if (strchr("rwa+b", *m)) hmode[n++] = *m;
    }
    hmode[n] = 0;
    if (!host_path(win, path, sizeof path) || !n) { crt_set_errno(c, EINVAL); return 0; }
    FILE *f = fopen(path, hmode);
    if (!f) { crt_set_errno(c, errno); return 0; }
    pthread_mutex_lock(&FILES_LOCK);
    int i = 3;
    while (i < MAX_FILES && FILES[i].f) i++;
    if (i < MAX_FILES) FILES[i].f = f, FILES[i].binary = binary;
    pthread_mutex_unlock(&FILES_LOCK);
    if (i == MAX_FILES) { fclose(f); crt_set_errno(c, EMFILE); return 0; }
    uint32_t gf = heap_calloc(1, FILE_SIZE);
    wr32(gf + F_FLAG, strchr(mode, '+') ? IORW : mode[0] == 'r' ? IOREAD : IOWRT);
    wr32(gf + F_FILE, i);
    return gf;
}
HOST_CDECL(msvcr120, fopen) { ret_i32(c, crt_fopen(c, ARG_STR(0), ARG_STR(1))); }
HOST_CDECL(msvcr120, _fsopen) { ret_i32(c, crt_fopen(c, ARG_STR(0), ARG_STR(1))); }  // sharing is ignored
HOST_CDECL(msvcr120, _wfsopen) {
    char name[4096], mode[64];
    if (!utf16_to_utf8(ARG(0), name, sizeof name) || !utf16_to_utf8(ARG(1), mode, sizeof mode))
        return crt_set_errno(c, EINVAL), ret_i32(c, 0);
    ret_i32(c, crt_fopen(c, name, mode));
}

HOST_CDECL(msvcr120, fclose) {
    uint32_t gf = ARG(0), i = rd32(gf + F_FILE);
    int r = fclose(hf(gf));
    if (i >= 3) {
        pthread_mutex_lock(&FILES_LOCK);
        FILES[i].f = NULL;
        pthread_mutex_unlock(&FILES_LOCK);
        heap_free(gf);
    }
    ret_i32(c, r ? 0xffffffffu : 0);
}
HOST_CDECL(msvcr120, fflush) { ret_i32(c, fflush(ARG(0) ? hf(ARG(0)) : NULL) ? 0xffffffffu : 0); }
HOST_CDECL(msvcr120, fwrite) { ret_i32(c, fwrite(ARG_PTR(0), ARG(1), ARG(2), hf(ARG(3)))); }
HOST_CDECL(msvcr120, fputs) { ret_i32(c, fputs(ARG_STR(0), hf(ARG(1))) < 0 ? 0xffffffffu : 0); }
HOST_CDECL(msvcr120, fputc) { ret_i32(c, (uint32_t)fputc((int)ARG(0), hf(ARG(1)))); }
HOST_CDECL(msvcr120, fgetc) { ret_i32(c, (uint32_t)fgetc(hf(ARG(0)))); }
HOST_CDECL(msvcr120, ungetc) { ret_i32(c, (uint32_t)ungetc((int)ARG(0), hf(ARG(1)))); }
HOST_CDECL(msvcr120, fseek) { ret_i32(c, fseeko(hf(ARG(0)), (int32_t)ARG(1), (int)ARG(2)) ? 0xffffffffu : 0); }
HOST_CDECL(msvcr120, _fseeki64) {  // (FILE *, __int64 offset, origin)
    ret_i32(c, fseeko(hf(ARG(0)), (int64_t)ARG_I64(1), (int)ARG(3)) ? 0xffffffffu : 0);
}
HOST_CDECL(msvcr120, fgetpos) {  // fpos_t is a 64-bit offset
    off_t p = ftello(hf(ARG(0)));
    if (p < 0) return ret_i32(c, 0xffffffffu);
    wr64(ARG(1), (uint64_t)p);
    ret_i32(c, 0);
}
HOST_CDECL(msvcr120, fsetpos) { ret_i32(c, fseeko(hf(ARG(0)), (off_t)rd64(ARG(1)), SEEK_SET) ? 0xffffffffu : 0); }
HOST_CDECL(msvcr120, setvbuf) {  // (FILE *, buf, mode, size): the guest buffer is not used
    int mode = ARG(2) == 4 ? _IONBF : ARG(2) == 0x40 ? _IOLBF : _IOFBF;
    ret_i32(c, setvbuf(hf(ARG(0)), NULL, mode, ARG(3)) ? 0xffffffffu : 0);
}
HOST_CDECL(msvcr120, _lock_file) { flockfile(hf(ARG(0))); }
HOST_CDECL(msvcr120, _unlock_file) { funlockfile(hf(ARG(0))); }

// Wide character I/O: binary streams hold UTF-16 code units; text streams hold bytes (the "C" locale
// maps them 1:1). WEOF is 0xFFFF.
HOST_CDECL(msvcr120, fgetwc) {
    uint32_t gf = ARG(0);
    FILE *f = hf(gf);
    int lo = fgetc(f);
    if (lo == EOF) return ret_i32(c, 0xffff);
    if (!FILES[rd32(gf + F_FILE)].binary) return ret_i32(c, (uint32_t)lo);
    int hi = fgetc(f);
    ret_i32(c, hi == EOF ? 0xffff : (uint32_t)(lo | hi << 8));
}
HOST_CDECL(msvcr120, fputwc) {
    uint32_t gf = ARG(1), ch = ARG(0) & 0xffff;
    FILE *f = hf(gf);
    int ok = FILES[rd32(gf + F_FILE)].binary ? fputc(ch & 0xff, f) != EOF && fputc(ch >> 8, f) != EOF
                                              : ch < 0x100 && fputc((int)ch, f) != EOF;
    ret_i32(c, ok ? ch : 0xffff);
}
HOST_CDECL(msvcr120, ungetwc) {
    uint32_t gf = ARG(1), ch = ARG(0) & 0xffff;
    FILE *f = hf(gf);
    if (ch == 0xffff) return ret_i32(c, 0xffff);
    int ok = FILES[rd32(gf + F_FILE)].binary ? ungetc(ch >> 8, f) != EOF && ungetc(ch & 0xff, f) != EOF
                                              : ch < 0x100 && ungetc((int)ch, f) != EOF;
    ret_i32(c, ok ? ch : 0xffff);
}

// printf formatting with guest varargs: `va` is the guest address of the first variadic argument.
// Follows MSVC: "l" is 32-bit, "I64"/"ll" 64-bit, "I"/"I32" 32-bit, %p is 8 upper-case hex digits,
// %e/%g exponents have at least three digits, %S/%ls take UTF-16 strings. Infinities and NaNs print the
// host way ("inf"/"nan"), not MSVC's "1.#INF00".
typedef struct { char *s; size_t len, cap; } Buf;
static void put(Buf *b, const char *s, size_t n) {
    if (b->len + n + 1 > b->cap) b->s = realloc(b->s, b->cap = 2 * (b->len + n + 1));
    memcpy(b->s + b->len, s, n);
    b->s[b->len += n] = 0;
}

static void exp3(char *s, int zero_pad) {  // "1e+05" -> "1e+005", keeping the field width if padded
    char *e = strpbrk(s, "eE");
    if (!e || !(e[1] == '+' || e[1] == '-') || strlen(e + 2) != 2) return;
    memmove(e + 3, e + 2, 3);
    e[2] = '0';
    if (s[0] == ' ') memmove(s, s + 1, strlen(s));  // right-justified: drop one pad space
    else if (s[strlen(s) - 1] == ' ') s[strlen(s) - 1] = 0;
    else if (zero_pad) {
        char *z = s + (*s == '-' || *s == '+' || *s == ' ');
        if (z[0] == '0' && z[1] >= '0' && z[1] <= '9') memmove(z, z + 1, strlen(z));
    }
}

char *crt_vformat(const char *f, uint32_t va, size_t *len) {
    Buf b = {0};
    put(&b, "", 0);
    while (*f) {
        const char *pct = strchr(f, '%');
        if (!pct) { put(&b, f, strlen(f)); break; }
        put(&b, f, pct - f);
        f = pct + 1;
        if (*f == '%') { put(&b, "%", 1); f++; continue; }
        char spec[64] = "%";
        size_t sn = 1;
        int zero = 0;
        while (*f && strchr("-+ #0", *f)) { if (*f == '0') zero = 1; spec[sn++] = *f++; }
        for (int part = 0; part < 2; part++) {  // width, then .precision
            if (part) { if (*f != '.') break; spec[sn++] = *f++; }
            if (*f == '*') {
                sn += snprintf(spec + sn, sizeof spec - sn, "%d", (int32_t)rd32(va));
                va += 4, f++;
            } else
                while (*f >= '0' && *f <= '9' && sn < 40) spec[sn++] = *f++;
        }
        int wide64 = 0, shrt = 0, wide = 0;
        if (!strncmp(f, "I64", 3)) wide64 = 1, f += 3;
        else if (!strncmp(f, "I32", 3)) f += 3;
        else if (!strncmp(f, "ll", 2)) wide64 = 1, f += 2;
        else if (*f == 'I' || *f == 'L' || *f == 'z' || *f == 't' || *f == 'j') f++;
        else if (!strncmp(f, "hh", 2)) shrt = 2, f += 2;
        else if (*f == 'h') shrt = 1, f++;
        else if (*f == 'l' || *f == 'w') wide = 1, f++;
        char conv = *f ? *f++ : 0, tmp[512], *out = tmp;
        int n = 0;
        switch (conv) {
        case 'd': case 'i': case 'u': case 'o': case 'x': case 'X': {
            int sgn = conv == 'd' || conv == 'i';
            int64_t v;
            if (wide64) v = (int64_t)rd64(va), va += 8;
            else {
                uint32_t u = rd32(va);
                va += 4;
                if (shrt == 1) u = sgn ? (uint32_t)(int16_t)u : (uint16_t)u;
                if (shrt == 2) u = sgn ? (uint32_t)(int8_t)u : (uint8_t)u;
                v = sgn ? (int64_t)(int32_t)u : (int64_t)u;
            }
            strcpy(spec + sn, sgn ? "lld" : (char[]){'l', 'l', conv, 0});
            n = snprintf(tmp, sizeof tmp, spec, v);
            break;
        }
        case 'p':
            n = snprintf(tmp, sizeof tmp, "%08X", rd32(va));
            va += 4;
            break;
        case 'c': case 'C': {
            uint32_t ch = rd32(va) & ((wide || conv == 'C') ? 0xffff : 0xff);
            va += 4;
            strcpy(spec + sn, "c");
            n = snprintf(tmp, sizeof tmp, spec, ch < 0x100 ? (int)ch : '?');
            break;
        }
        case 's': case 'S': {
            uint32_t p = rd32(va);
            va += 4;
            char *s = "(null)", *conv8 = NULL;
            if (p && (wide || conv == 'S')) {
                size_t cap = 4 * 65536;
                conv8 = malloc(cap);
                s = utf16_to_utf8(p, conv8, cap) ? conv8 : "?";
            } else if (p) s = (char *)P(p);
            strcpy(spec + sn, "s");
            n = snprintf(NULL, 0, spec, s);
            out = n < (int)sizeof tmp ? tmp : malloc(n + 1);
            snprintf(out, n + 1, spec, s);
            free(conv8);
            break;
        }
        case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': case 'a': case 'A': {
            double v = rdf64(va);
            va += 8;
            spec[sn] = conv, spec[sn + 1] = 0;
            n = snprintf(NULL, 0, spec, v) + 2;
            out = n < (int)sizeof tmp ? tmp : malloc(n + 1);
            snprintf(out, n + 1, spec, v);
            if (conv != 'f' && conv != 'F' && conv != 'a' && conv != 'A') exp3(out, zero);
            n = (int)strlen(out);
            break;
        }
        case 'n':
            wr32(rd32(va), (uint32_t)b.len);
            va += 4;
            break;
        default:  // unknown conversion: MSVC's invalid parameter handler; print it literally
            put(&b, pct, f - pct);
            break;
        }
        put(&b, out, n);
        if (out != tmp) free(out);
    }
    *len = b.len;
    return b.s;
}

// Write formatted output into guest buffer `buf` of `size` bytes, like _vsnprintf: returns the length,
// or -1 if it didn't fit (then the buffer holds as much as fits, not NUL-terminated if full).
static uint32_t vsnprintf_guest(uint32_t buf, uint32_t size, const char *f, uint32_t va) {
    size_t n;
    char *s = crt_vformat(f, va, &n);
    uint32_t r = n <= size ? (uint32_t)n : 0xffffffffu;
    memcpy(P(buf), s, n < size ? n + 1 : size);
    free(s);
    return r;
}

static uint32_t fprintf_host(FILE *f, const char *fmt, uint32_t va) {
    size_t n;
    char *s = crt_vformat(fmt, va, &n);
    size_t w = fwrite(s, 1, n, f);
    free(s);
    return w == n ? (uint32_t)n : 0xffffffffu;
}
HOST_CDECL(msvcr120, printf) { ret_i32(c, fprintf_host(stdout, ARG_STR(0), ARG_ADDR(1))); }
HOST_CDECL(msvcr120, fprintf) { ret_i32(c, fprintf_host(hf(ARG(0)), ARG_STR(1), ARG_ADDR(2))); }
HOST_CDECL(msvcr120, _vsnprintf) { ret_i32(c, vsnprintf_guest(ARG(0), ARG(1), ARG_STR(2), ARG(3))); }
HOST_CDECL(msvcr120, vsprintf) { ret_i32(c, vsnprintf_guest(ARG(0), 0x7fffffff, ARG_STR(1), ARG(2))); }
HOST_CDECL(msvcr120, sprintf_s) {  // (buf, size, fmt, ...): on overflow, an empty string and -1
    uint32_t r = vsnprintf_guest(ARG(0), ARG(1), ARG_STR(2), ARG_ADDR(3));
    if (r == 0xffffffffu || r == ARG(1)) {
        if (ARG(1)) wr8(ARG(0), 0);
        r = 0xffffffffu;
    }
    ret_i32(c, r);
}

// File system.
HOST_CDECL(msvcr120, _getcwd) {  // (buf, size): the Windows path; a NULL buf is malloc'd (at least size bytes)
    char host[4096], win[4096];
    if (!getcwd(host, sizeof host) || !win_path(host, win, sizeof win)) return crt_set_errno(c, ERANGE), ret_i32(c, 0);
    uint32_t n = strlen(win) + 1, buf = ARG(0), size = ARG(1);
    if (!buf) buf = heap_alloc(n > size ? n : size);
    else if (n > size) return crt_set_errno(c, ERANGE), ret_i32(c, 0);
    memcpy(P(buf), win, n);
    ret_i32(c, buf);
}

// _findfirst64i32(pattern, _finddata64i32_t *): the first entry matching the last component (case-insensitive
// wildcards). The game only uses it as an existence test (ModDoesFileExist) and never calls _findnext or
// _findclose, so the handle is a dummy. _finddata64i32_t: attrib, then (8-aligned) time_create/access/write as
// 64-bit, size at 32, name[260] at 36.
HOST_CDECL(msvcr120, _findfirst64i32) {
    enum { A_RDONLY = 0x1, A_SUBDIR = 0x10 };
    char path[4096], dir[4096], name[1024] = "";
    if (!host_path(ARG_STR(0), path, sizeof path)) return crt_set_errno(c, EINVAL), ret_i32(c, 0xffffffffu);
    char *slash = strrchr(path, '/');
    snprintf(dir, sizeof dir, "%.*s", slash ? (int)(slash - path) : 1, slash ? path : ".");
    const char *pat = slash ? slash + 1 : path;
    if (!strpbrk(pat, "*?")) {
        struct stat st;
        if (!stat(path, &st)) snprintf(name, sizeof name, "%s", pat);
    } else {
        DIR *d = opendir(dir);
        for (struct dirent *e; d && (e = readdir(d));)
            if (!fnmatch(pat, e->d_name, FNM_CASEFOLD)) { snprintf(name, sizeof name, "%s", e->d_name); break; }
        if (d) closedir(d);
    }
    struct stat st;
    snprintf(path, sizeof path, "%s/%s", dir, name);
    if (!*name || stat(path, &st)) return crt_set_errno(c, ENOENT), ret_i32(c, 0xffffffffu);
    uint32_t fd = ARG(1);
    wr32(fd, (S_ISDIR(st.st_mode) ? A_SUBDIR : 0) | (st.st_mode & S_IWUSR ? 0 : A_RDONLY));
    wr64(fd + 8, (uint64_t)st.st_birthtimespec.tv_sec);
    wr64(fd + 16, (uint64_t)st.st_atimespec.tv_sec);
    wr64(fd + 24, (uint64_t)st.st_mtimespec.tv_sec);
    wr32(fd + 32, (uint32_t)st.st_size);
    snprintf((char *)P(fd + 36), 260, "%s", name);
    ret_i32(c, 1);
}
