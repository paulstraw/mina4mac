// WS2_32, offline: WSAStartup fails (WSASYSNOTREADY), so the game's Socket class (0x4326d0, used by streaming
// integrations) never gets a socket, and the rest fail with WSANOTINITIALISED. WSAGetLastError is the thread's
// last error, as on Windows.
#include "host.h"
#include "proc.h"

enum { WSASYSNOTREADY = 10091, WSANOTINITIALISED = 10093, SOCKET_ERROR = 0xffffffffu };

static void fail(CPU *c, uint32_t err, uint32_t ret) {
    wr32(c->fs_base + TEB_LAST_ERROR, err);
    ret_i32(c, ret);
}
HOST_STDCALL(ws2_32, WSAStartup, 8) { ret_i32(c, WSASYSNOTREADY); }  // returns the error; doesn't set it
HOST_STDCALL(ws2_32, WSACleanup, 0) { fail(c, WSANOTINITIALISED, SOCKET_ERROR); }
HOST_STDCALL(ws2_32, WSAGetLastError, 0) { ret_i32(c, rd32(c->fs_base + TEB_LAST_ERROR)); }
HOST_STDCALL(ws2_32, socket, 12) { fail(c, WSANOTINITIALISED, SOCKET_ERROR); }  // INVALID_SOCKET
HOST_STDCALL(ws2_32, closesocket, 4) { fail(c, WSANOTINITIALISED, SOCKET_ERROR); }
HOST_STDCALL(ws2_32, setsockopt, 20) { fail(c, WSANOTINITIALISED, SOCKET_ERROR); }
HOST_STDCALL(ws2_32, ioctlsocket, 12) { fail(c, WSANOTINITIALISED, SOCKET_ERROR); }
HOST_STDCALL(ws2_32, connect, 12) { fail(c, WSANOTINITIALISED, SOCKET_ERROR); }
HOST_STDCALL(ws2_32, send, 16) { fail(c, WSANOTINITIALISED, SOCKET_ERROR); }
HOST_STDCALL(ws2_32, recv, 16) { fail(c, WSANOTINITIALISED, SOCKET_ERROR); }
HOST_STDCALL(ws2_32, gethostbyname, 4) { fail(c, WSANOTINITIALISED, 0); }
HOST_STDCALL(ws2_32, htons, 4) { ret_i32(c, (uint16_t)(ARG(0) << 8 | (ARG(0) >> 8 & 0xff))); }
