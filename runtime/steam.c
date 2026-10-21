// Steamworks (steam_api.dll, SDK 1.45/1.46) stubbed natively (HLE), like galaxy.c: the Steam build runs offline.
// SteamAPI_Init fails and every interface accessor yields NULL.
#include "host.h"

HOST_CDECL(steam_api, SteamAPI_RestartAppIfNecessary) { ret_i32(c, 0); }  // never relaunch through Steam
HOST_CDECL(steam_api, SteamAPI_Init) { ret_i32(c, 0); }
HOST_CDECL(steam_api, SteamAPI_Shutdown) {}
HOST_CDECL(steam_api, SteamAPI_RunCallbacks) {}
HOST_CDECL(steam_api, SteamAPI_RegisterCallback) {}
HOST_CDECL(steam_api, SteamAPI_UnregisterCallback) {}
HOST_CDECL(steam_api, SteamAPI_GetHSteamUser) { ret_i32(c, 0); }
HOST_CDECL(steam_api, SteamInternal_FindOrCreateUserInterface) { ret_i32(c, 0); }
// ContextInit(ctx), ctx = {init function, counter, interface pointer}, returns &ctx[2]. The init function
// (which would call FindOrCreateUserInterface) is never run, so the interface pointer stays NULL.
HOST_CDECL(steam_api, SteamInternal_ContextInit) { ret_i32(c, ARG(0) + 8); }
