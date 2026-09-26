// KERNEL32 file-system functions implemented natively (HLE). Paths are Windows paths (hle.h host_path);
// the host file system is case-insensitive (APFS default), as the game expects.
#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "hle.h"
#include "host.h"
#include "kernel32.h"
#include "proc.h"

enum { INVALID_HANDLE = 0xffffffffu, INVALID_ATTRIBUTES = 0xffffffffu };
enum { FILE_ATTRIBUTE_READONLY = 0x1, FILE_ATTRIBUTE_DIRECTORY = 0x10, FILE_ATTRIBUTE_NORMAL = 0x80 };
enum {
    ERROR_FILE_NOT_FOUND = 2, ERROR_PATH_NOT_FOUND = 3, ERROR_ACCESS_DENIED = 5, ERROR_INVALID_HANDLE = 6,
    ERROR_NO_MORE_FILES = 18, ERROR_FILE_EXISTS = 80, ERROR_ALREADY_EXISTS = 183, ERROR_DIR_NOT_EMPTY = 145,
};
enum { MOVEFILE_REPLACE_EXISTING = 0x1 };

static void set_error(CPU *c, uint32_t e) { wr32(c->fs_base + TEB_LAST_ERROR, e); }

static uint32_t win_error(int e) {  // host errno -> Windows error code
    switch (e) {
    case ENOENT: return ERROR_FILE_NOT_FOUND;
    case ENOTDIR: return ERROR_PATH_NOT_FOUND;
    case EACCES: case EPERM: return ERROR_ACCESS_DENIED;
    case EEXIST: return ERROR_ALREADY_EXISTS;
    case ENOTEMPTY: return ERROR_DIR_NOT_EMPTY;
    default: return ERROR_ACCESS_DENIED;
    }
}

// Guest path argument (UTF-16 or ANSI, taken as UTF-8) to a host path. 0 if it doesn't fit.
static int path_w(uint32_t ws, char *host, size_t n) {
    char win[4096];
    return ws && utf16_to_utf8(ws, win, sizeof win) && host_path(win, host, n);
}
static int path_a(uint32_t s, char *host, size_t n) { return s && host_path((const char *)P(s), host, n); }

static uint32_t attributes(const struct stat *st) {
    uint32_t a = S_ISDIR(st->st_mode) ? FILE_ATTRIBUTE_DIRECTORY : 0;
    if (!(st->st_mode & S_IWUSR)) a |= FILE_ATTRIBUTE_READONLY;
    return a ? a : FILE_ATTRIBUTE_NORMAL;
}

static uint64_t filetime(struct timespec t) {  // 100 ns units since 1601
    return ((uint64_t)t.tv_sec + 11644473600u) * 10000000 + t.tv_nsec / 100;
}

// FindFirstFileW/FindNextFileW: a directory stream and the pattern (last path component, matched
// case-insensitively). "." and ".." are reported like on Windows.
typedef struct { DIR *dir; char path[4096], pattern[1024]; } Find;

// Fill WIN32_FIND_DATAW (592 bytes) at `fd` with the next match; 0 at the end of the directory.
static int find_next(Find *f, uint32_t fd) {
    for (struct dirent *e; (e = readdir(f->dir));) {
        if (fnmatch(f->pattern, e->d_name, FNM_CASEFOLD)) continue;
        char full[4096 + 256];
        struct stat st;
        snprintf(full, sizeof full, "%s/%s", f->path, e->d_name);
        if (stat(full, &st)) continue;  // e.g. a dangling symlink
        memset(P(fd), 0, 592);
        wr32(fd, attributes(&st));
        wr64(fd + 4, filetime(st.st_birthtimespec));
        wr64(fd + 12, filetime(st.st_atimespec));
        wr64(fd + 20, filetime(st.st_mtimespec));
        wr32(fd + 28, (uint64_t)st.st_size >> 32);
        wr32(fd + 32, (uint32_t)st.st_size);
        if (utf8_to_utf16(e->d_name, fd + 44, 260) > 259) continue;  // too long for cFileName
        return 1;
    }
    return 0;
}

static uint32_t find_first(CPU *c, const char *host, uint32_t fd) {
    Find *f = calloc(1, sizeof *f);
    const char *slash = strrchr(host, '/');
    if (slash) {
        snprintf(f->path, sizeof f->path, "%.*s", (int)(slash - host), host);
        if (!f->path[0]) strcpy(f->path, "/");
    } else strcpy(f->path, ".");
    snprintf(f->pattern, sizeof f->pattern, "%s", slash ? slash + 1 : host);
    // Windows' "*.*" matches every name, with or without a dot.
    if (!strcmp(f->pattern, "*.*")) strcpy(f->pattern, "*");
    uint32_t h;
    if (!(f->dir = opendir(f->path))) set_error(c, errno == ENOENT ? ERROR_PATH_NOT_FOUND : win_error(errno)), h = 0;
    else if (!find_next(f, fd)) set_error(c, ERROR_FILE_NOT_FOUND), h = 0;
    else if (!(h = handle_new(HK_FIND, (uintptr_t)f))) set_error(c, ERROR_ACCESS_DENIED);
    if (h) return h;
    if (f->dir) closedir(f->dir);
    free(f);
    return INVALID_HANDLE;
}

HOST_STDCALL(kernel32, FindFirstFileW, 8) {  // (pattern, WIN32_FIND_DATAW *)
    char host[4096];
    if (!path_w(ARG(0), host, sizeof host)) return set_error(c, ERROR_PATH_NOT_FOUND), ret_i32(c, INVALID_HANDLE);
    ret_i32(c, find_first(c, host, ARG(1)));
}

// FindFirstFileExW(pattern, info level, WIN32_FIND_DATAW *, search op, filter, flags): the basic and
// standard info levels fill the same fields (the basic one leaves cAlternateFileName empty, as we do).
HOST_STDCALL(kernel32, FindFirstFileExW, 24) {
    char host[4096];
    if (!path_w(ARG(0), host, sizeof host)) return set_error(c, ERROR_PATH_NOT_FOUND), ret_i32(c, INVALID_HANDLE);
    ret_i32(c, find_first(c, host, ARG(2)));
}

HOST_STDCALL(kernel32, FindNextFileW, 8) {
    uint64_t f;
    if (!handle_get(ARG(0), HK_FIND, &f)) return set_error(c, ERROR_INVALID_HANDLE), ret_i32(c, 0);
    if (!find_next((Find *)(uintptr_t)f, ARG(1))) return set_error(c, ERROR_NO_MORE_FILES), ret_i32(c, 0);
    ret_i32(c, 1);
}

HOST_STDCALL(kernel32, FindClose, 4) {
    uint64_t f;
    if (!handle_get(ARG(0), HK_FIND, &f) || !handle_close(ARG(0)))
        return set_error(c, ERROR_INVALID_HANDLE), ret_i32(c, 0);
    closedir(((Find *)(uintptr_t)f)->dir);
    free((Find *)(uintptr_t)f);
    ret_i32(c, 1);
}

static uint32_t get_attributes(CPU *c, const char *host) {
    struct stat st;
    if (stat(host, &st)) return set_error(c, win_error(errno)), INVALID_ATTRIBUTES;
    return attributes(&st);
}
HOST_STDCALL(kernel32, GetFileAttributesA, 4) {
    char host[4096];
    if (!path_a(ARG(0), host, sizeof host)) return set_error(c, ERROR_PATH_NOT_FOUND), ret_i32(c, INVALID_ATTRIBUTES);
    ret_i32(c, get_attributes(c, host));
}
HOST_STDCALL(kernel32, GetFileAttributesW, 4) {
    char host[4096];
    if (!path_w(ARG(0), host, sizeof host)) return set_error(c, ERROR_PATH_NOT_FOUND), ret_i32(c, INVALID_ATTRIBUTES);
    ret_i32(c, get_attributes(c, host));
}

// Returns BOOL, setting the last error from errno on failure.
static uint32_t check(CPU *c, int failed) {
    if (failed) set_error(c, win_error(errno));
    return !failed;
}

HOST_STDCALL(kernel32, CreateDirectoryA, 8) {  // (path, security)
    char host[4096];
    if (!path_a(ARG(0), host, sizeof host)) return set_error(c, ERROR_PATH_NOT_FOUND), ret_i32(c, 0);
    ret_i32(c, check(c, mkdir(host, 0755) != 0));
}
HOST_STDCALL(kernel32, CreateDirectoryW, 8) {
    char host[4096];
    if (!path_w(ARG(0), host, sizeof host)) return set_error(c, ERROR_PATH_NOT_FOUND), ret_i32(c, 0);
    ret_i32(c, check(c, mkdir(host, 0755) != 0));
}

HOST_STDCALL(kernel32, DeleteFileW, 4) {
    char host[4096];
    if (!path_w(ARG(0), host, sizeof host)) return set_error(c, ERROR_PATH_NOT_FOUND), ret_i32(c, 0);
    ret_i32(c, check(c, unlink(host) != 0));
}

// MoveFileExW(from, to, flags): rename, failing if `to` exists unless MOVEFILE_REPLACE_EXISTING.
HOST_STDCALL(kernel32, MoveFileExW, 12) {
    char from[4096], to[4096];
    struct stat st;
    if (!path_w(ARG(0), from, sizeof from) || !path_w(ARG(1), to, sizeof to))
        return set_error(c, ERROR_PATH_NOT_FOUND), ret_i32(c, 0);
    if (!(ARG(2) & MOVEFILE_REPLACE_EXISTING) && !stat(to, &st)) return set_error(c, ERROR_ALREADY_EXISTS), ret_i32(c, 0);
    ret_i32(c, check(c, rename(from, to) != 0));
}

// CopyFileA(from, to, fail if exists).
static uint32_t copy_file(CPU *c, const char *from, const char *to, int fail_if_exists) {
    FILE *in = fopen(from, "rb");
    if (!in) return check(c, 1);
    FILE *out = fopen(to, fail_if_exists ? "wbx" : "wb");
    if (!out) {
        int e = errno;
        fclose(in);
        errno = e;
        return set_error(c, e == EEXIST ? ERROR_FILE_EXISTS : win_error(e)), 0;
    }
    char buf[65536];
    size_t n;
    int ok = 1;
    while ((n = fread(buf, 1, sizeof buf, in))) ok &= fwrite(buf, 1, n, out) == n;
    ok &= !ferror(in);
    fclose(in);
    ok &= !fclose(out);
    if (!ok) set_error(c, ERROR_ACCESS_DENIED);
    return ok;
}
HOST_STDCALL(kernel32, CopyFileA, 12) {
    char from[4096], to[4096];
    if (!path_a(ARG(0), from, sizeof from) || !path_a(ARG(1), to, sizeof to)) return set_error(c, ERROR_PATH_NOT_FOUND), ret_i32(c, 0);
    ret_i32(c, copy_file(c, from, to, ARG(2)));
}
HOST_STDCALL(kernel32, CopyFileW, 12) {
    char from[4096], to[4096];
    if (!path_w(ARG(0), from, sizeof from) || !path_w(ARG(1), to, sizeof to)) return set_error(c, ERROR_PATH_NOT_FOUND), ret_i32(c, 0);
    ret_i32(c, copy_file(c, from, to, ARG(2)));
}
