// The mina4mac launcher: map noita.exe and the recompiled DLLs (msvcp120.dll) from the game directory, set
// up the guest process and main thread, bind imports (to recompiled DLLs' exports, otherwise to host
// thunks), run the DLLs' entry points (DllMain) and then the exe's recompiled entry point.
//   mina4mac [args]     (game files from $NOITA_DIR, default build/game; args go to the game)
//   MINA4MAC_TRACE=1    log every host import call to stderr
//   MINA4MAC_TRACE=a,b  only the imports whose dll!name contains a or b (e.g. SDL_SetWindow,glViewport)
//   MINA4MAC_COUNT_IMPORTS=<file>   write per-import call counts to <file> at exit
//   MINA4MAC_JOBLOG=<file>          log the job system (worker jobs, main-thread waits), see joblog.c
//   MINA4MAC_CPUS, MINA4MAC_QOS, MINA4MAC_YIELD   scheduling knobs, see sched.h
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "host.h"
#include "joblog.h"
#include "msvcr120.h"
#include "proc.h"
#include "rt.h"
#include "sched.h"

enum { EXE_BASE = 0x400000, EXE_ENTRY = 0xdfadb0, EXIT_RET = 0x0badf000 };

// Recompiled DLLs, in load order, at their bases from tools/pe.py MODULES.
static const struct { const char *file; uint32_t base, entry; } DLLS[] = {
    {"msvcp120.dll", 0x18000000, 0x1803b707},
};
enum { NDLLS = sizeof DLLS / sizeof *DLLS, DLL_PROCESS_ATTACH = 1 };

int main(int argc, char **argv) {
    const char *dir = getenv("NOITA_DIR");
    if (!dir || !*dir) dir = "build/game";
    char path[4096];
    snprintf(path, sizeof path, "%s/noita.exe", dir);
    const char *trace = getenv("MINA4MAC_TRACE");
    rt_trace = trace && *trace && strcmp(trace, "0");
    if (rt_trace && strcmp(trace, "1")) rt_trace_filter = trace;
    const char *counts = getenv("MINA4MAC_COUNT_IMPORTS");
    if (counts && *counts) rt_count_imports(counts);

    sched_init();
    rt_init();
    uint32_t entry = rt_map_pe(path, EXE_BASE);
    if (entry != EXE_ENTRY) {  // the recompiled code is for one specific build of the game
        fprintf(stderr, "%s: entry point %#x, expected %#x (a different noita.exe build?)\n", path, entry, EXE_ENTRY);
        return 2;
    }
    for (int i = 0; i < NDLLS; i++) {
        char dll[4096];
        snprintf(dll, sizeof dll, "%s/%s", dir, DLLS[i].file);
        if (rt_map_pe(dll, DLLS[i].base) != DLLS[i].entry) {
            fprintf(stderr, "%s: unexpected entry point (a different build?)\n", dll);
            return 2;
        }
        rt_register_module(DLLS[i].file, DLLS[i].base);
    }
    rt_process_init(EXE_BASE);
    CPU c = {.fpu_cw = 0x27f};  // Windows default: 53-bit precision, round-to-nearest, all masked
    rt_thread_init(&c);
    int slots = rt_bind_imports(EXE_BASE);
    for (int i = 0; i < NDLLS; i++) slots += rt_bind_imports(DLLS[i].base);
    joblog_init(EXE_BASE);
    crt_init(argc, argv);
    if (chdir(dir)) { perror(dir); return 2; }  // the game finds data/ etc. relative to its own directory
    if (rt_trace) fprintf(stderr, "[mina4mac] mapped %s, bound %d import slots, entry %#x\n", path, slots, entry);

    for (int i = 0; i < NDLLS; i++) {  // DllMain(hinstDLL, DLL_PROCESS_ATTACH, lpReserved)
        uint32_t args[] = {DLLS[i].base, DLL_PROCESS_ATTACH, 1};  // non-NULL: a static load
        if (rt_trace) fprintf(stderr, "[mina4mac] %s DllMain\n", DLLS[i].file);
        if (!call_guest(&c, DLLS[i].entry, 3, args)) {
            fprintf(stderr, "%s: DllMain failed\n", DLLS[i].file);
            return 2;
        }
    }

    // The entry point takes no arguments; it returns only if the CRT's exit path does.
    c.esp -= 4;
    wr32(c.esp, EXIT_RET);
    guest_call(&c, entry);
    fprintf(stderr, "[mina4mac] entry point returned %#x\n", c.eax);
    return (int)c.eax;
}
