// USER32 and COMDLG32: there are no native Windows windows (SDL owns the only one), so the game's file
// dialog (0x84b9e0, a dev tool) owns nothing and is cancelled.
#include "host.h"

HOST_STDCALL(user32, GetActiveWindow, 0) { ret_i32(c, 0); }
// GetOpenFileNameA(OPENFILENAMEA *): FALSE with CommDlgExtendedError() 0 means the user cancelled.
HOST_STDCALL(comdlg32, GetOpenFileNameA, 4) { ret_i32(c, 0); }
