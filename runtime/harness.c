// Differential-test harness: load a memory/register snapshot, run one recompiled function,
// write back the resulting registers and memory regions.
//   harness <snapshot-in> <result-out> [<image.bin> <base-hex>]
#include <stdio.h>
#include <stdlib.h>

#include "rt.h"

typedef struct { uint32_t addr, len; } Region;

int main(int argc, char **argv) {
    if (argc != 3 && argc != 5) return 2;
    rt_init();
    if (argc == 5) rt_load_image(argv[3], (uint32_t)strtoul(argv[4], NULL, 16));
    FILE *f = fopen(argv[1], "rb");
    uint32_t func, nreg;
    CPU c = {.fpu_cw = 0x27f};  // Windows default: 53-bit precision, round-to-nearest, all masked
    fread(&func, 4, 1, f);
    fread(&c.eax, 4, 9, f);  // eax..edi, fs_base
    fread(c.xmm, 16, 8, f);
    fread(&nreg, 4, 1, f);
    Region regs[64];
    for (uint32_t i = 0; i < nreg; i++) {
        fread(&regs[i], 8, 1, f);
        fread(MEM + regs[i].addr, 1, regs[i].len, f);
    }
    fclose(f);
    GuestFn fn = rt_lookup(func);
    if (!fn) { fprintf(stderr, "no function %#x\n", func); return 5; }
    fn(&c);
    FILE *o = fopen(argv[2], "wb");
    fwrite(&c.eax, 4, 9, o);
    fwrite(c.xmm, 16, 8, o);
    for (uint32_t i = 0; i < nreg; i++) fwrite(MEM + regs[i].addr, 1, regs[i].len, o);
    fclose(o);
    return 0;
}
