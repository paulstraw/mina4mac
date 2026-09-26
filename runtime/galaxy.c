// GOG Galaxy SDK (Galaxy.dll) stubbed natively (HLE): the game always runs offline. Init and ProcessData do
// nothing, and the interface accessors return NULL, which noita.exe checks before every use (it then logs
// and skips achievements, stats and sign-in).
#include "host.h"

HOST(Galaxy, Init, "?Init@api@galaxy@@YAXABUInitOptions@12@@Z", 0) {}
HOST(Galaxy, ProcessData, "?ProcessData@api@galaxy@@YAXXZ", 0) {}
HOST(Galaxy, Shutdown, "?Shutdown@api@galaxy@@YAXXZ", 0) {}
HOST(Galaxy, User, "?User@api@galaxy@@YAPAVIUser@12@XZ", 0) { ret_i32(c, 0); }
HOST(Galaxy, Stats, "?Stats@api@galaxy@@YAPAVIStats@12@XZ", 0) { ret_i32(c, 0); }
HOST(Galaxy, ListenerRegistrar, "?ListenerRegistrar@api@galaxy@@YAPAVIListenerRegistrar@12@XZ", 0) { ret_i32(c, 0); }
