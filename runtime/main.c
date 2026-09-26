// The noitamac launcher: map noita.exe from the game directory, set up the guest process and main
// thread, bind its imports to host thunks and run the recompiled entry point.
//   noitamac [args]     (game files from $NOITA_DIR, default build/game; args go to the game)
//   NOITAMAC_TRACE=1    log every host import call to stderr
#include <stdio.h>
#include <stdlib.h>

#include "msvcr120.h"
#include "proc.h"
#include "rt.h"

enum { EXE_BASE = 0x400000, EXE_ENTRY = 0xdfadb0, EXIT_RET = 0x0badf000 };

int main(int argc, char **argv) {
    const char *dir = getenv("NOITA_DIR");
    char path[4096];
    snprintf(path, sizeof path, "%s/noita.exe", dir && *dir ? dir : "build/game");
    const char *trace = getenv("NOITAMAC_TRACE");
    rt_trace = trace && *trace && *trace != '0';

    rt_init();
    uint32_t entry = rt_map_pe(path, EXE_BASE);
    if (entry != EXE_ENTRY) {  // the recompiled code is for one specific build of the game
        fprintf(stderr, "%s: entry point %#x, expected %#x (a different noita.exe build?)\n", path, entry, EXE_ENTRY);
        return 2;
    }
    rt_process_init(EXE_BASE);
    CPU c = {.fpu_cw = 0x27f};  // Windows default: 53-bit precision, round-to-nearest, all masked
    rt_thread_init(&c);
    int slots = rt_bind_imports(EXE_BASE);
    crt_init(argc, argv);
    if (rt_trace) fprintf(stderr, "[noitamac] mapped %s, bound %d import slots, entry %#x\n", path, slots, entry);

    // The entry point takes no arguments; it returns only if the CRT's exit path does.
    c.esp -= 4;
    wr32(c.esp, EXIT_RET);
    guest_call(&c, entry);
    fprintf(stderr, "[noitamac] entry point returned %#x\n", c.eax);
    return (int)c.eax;
}
