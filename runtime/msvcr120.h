// MSVCR120 HLE state that the launcher sets up (msvcr120.c).
#pragma once

// Set the CRT's view of the process: the command line (_acmdln, __getmainargs) built from argv, with
// argv[0] standing for the exe, and the data imports (_fmode, _commode). Call after rt_bind_imports.
void crt_init(int argc, char **argv);
