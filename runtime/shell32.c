// SHELL32 and ole32 implemented natively (HLE).
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "heap.h"
#include "hle.h"
#include "host.h"

enum { S_OK = 0, E_FAIL = 0x80004005, E_INVALIDARG = 0x80070057, KF_FLAG_CREATE = 0x8000 };
enum { ERROR_SUCCESS = 0, ERROR_PATH_NOT_FOUND = 3, ERROR_FILE_EXISTS = 80, ERROR_ALREADY_EXISTS = 183 };

// mkdir -p. Returns a Windows error code (SHCreateDirectoryEx's convention).
static uint32_t mkdirs(const char *host) {
    struct stat st;
    if (!stat(host, &st)) return S_ISDIR(st.st_mode) ? ERROR_ALREADY_EXISTS : ERROR_FILE_EXISTS;
    char p[4096];
    snprintf(p, sizeof p, "%s", host);
    for (char *s = p + 1; *s; s++)
        if (*s == '/') {
            *s = 0;
            if (mkdir(p, 0755) && errno != EEXIST) return ERROR_PATH_NOT_FOUND;
            *s = '/';
        }
    return mkdir(p, 0755) && errno != EEXIST ? ERROR_PATH_NOT_FOUND : ERROR_SUCCESS;
}

// Known folders live under ~/Library/Application Support/noitamac/, laid out like a Windows profile. Noita
// only asks for LocalLow (its saves go in LocalLow\Nolla_Games_Noita).
static const struct { uint8_t guid[16]; const char *dir; } KNOWN[] = {
    // FOLDERID_LocalAppDataLow {A520A1A4-1780-4FF6-BD18-167343C5AF16}
    {{0xa4, 0xa1, 0x20, 0xa5, 0x80, 0x17, 0xf6, 0x4f, 0xbd, 0x18, 0x16, 0x73, 0x43, 0xc5, 0xaf, 0x16}, "AppData/LocalLow"},
};

// SHGetKnownFolderPath(rfid, flags, token, &path): *path is a CoTaskMemAlloc'd (guest heap) wide string.
HOST_STDCALL(shell32, SHGetKnownFolderPath, 16) {
    uint32_t out = ARG(3);
    wr32(out, 0);
    for (size_t i = 0; i < sizeof KNOWN / sizeof *KNOWN; i++) {
        if (memcmp(ARG_PTR(0), KNOWN[i].guid, 16)) continue;
        const char *home = getenv("HOME");
        char host[4096], win[4096];
        snprintf(host, sizeof host, "%s/Library/Application Support/noitamac/%s", home ? home : "", KNOWN[i].dir);
        if ((ARG(1) & KF_FLAG_CREATE) && mkdirs(host) > ERROR_ALREADY_EXISTS) return ret_i32(c, E_FAIL);
        if (!win_path(host, win, sizeof win)) return ret_i32(c, E_FAIL);
        uint32_t n = utf8_to_utf16(win, 0, 0), p = heap_alloc(2 * n);
        utf8_to_utf16(win, p, n);
        wr32(out, p);
        return ret_i32(c, S_OK);
    }
    uint8_t *g = ARG_PTR(0);
    fprintf(stderr, "SHGetKnownFolderPath: unknown folder %02x%02x%02x%02x-...\n", g[3], g[2], g[1], g[0]);
    ret_i32(c, E_INVALIDARG);
}

// SHCreateDirectoryExW(hwnd, path, security): creates intermediate directories too.
HOST_STDCALL(shell32, SHCreateDirectoryExW, 12) {
    char win[4096], host[4096];
    if (!utf16_to_utf8(ARG(1), win, sizeof win) || !host_path(win, host, sizeof host)) return ret_i32(c, ERROR_PATH_NOT_FOUND);
    ret_i32(c, mkdirs(host));
}

HOST_STDCALL(ole32, CoTaskMemFree, 4) { heap_free(ARG(0)); }
