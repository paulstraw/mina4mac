// PE loader test (built and run by tools/loadtest.py): map a module with rt_map_pe and compare every byte
// with the image tools/pe.py produces for the same base (pefile's mapping plus its own relocation pass).
//   load_test <module file> <base-hex> <expected entry-hex> <pe.py image>
#include <stdio.h>
#include <stdlib.h>

#include "rt.h"

const FnEntry FN_TABLE[1];
const int FN_COUNT = 0;

int main(int argc, char **argv) {
    if (argc != 5) { fprintf(stderr, "usage: load_test <file> <base> <entry> <image>\n"); return 2; }
    uint32_t base = strtoul(argv[2], NULL, 16), want_entry = strtoul(argv[3], NULL, 16);
    rt_init();
    uint32_t entry = rt_map_pe(argv[1], base);
    FILE *f = fopen(argv[4], "rb");
    if (!f) { perror(argv[4]); return 2; }
    uint32_t n = 0, diffs = 0, first = 0;
    for (int ch; (ch = fgetc(f)) != EOF; n++)
        if (rd8(base + n) != ch && !diffs++) first = base + n;
    fclose(f);
    printf("%-16s base %#010x: %u bytes, %u differ%s", argv[1], base, n, diffs, diffs ? "" : "\n");
    if (diffs) printf(" (first at %#x)\n", first);
    if (entry != want_entry) printf("  entry %#x, want %#x\n", entry, want_entry);
    return diffs || entry != want_entry;
}
