// WININET, offline: opening a session fails (ERROR_INTERNET_NAME_NOT_RESOLVED), so the game never gets a
// handle to pass to the rest, which fail too.
#include "host.h"
#include "proc.h"

enum { ERROR_INTERNET_NAME_NOT_RESOLVED = 12007 };

static void fail(CPU *c) {
    wr32(c->fs_base + TEB_LAST_ERROR, ERROR_INTERNET_NAME_NOT_RESOLVED);
    ret_i32(c, 0);
}
HOST_STDCALL(wininet, InternetOpenA, 20) { fail(c); }
HOST_STDCALL(wininet, InternetConnectA, 32) { fail(c); }
HOST_STDCALL(wininet, HttpOpenRequestA, 32) { fail(c); }
HOST_STDCALL(wininet, HttpSendRequestA, 20) { fail(c); }
HOST_STDCALL(wininet, InternetReadFile, 16) { fail(c); }
HOST_STDCALL(wininet, InternetCloseHandle, 4) { fail(c); }
