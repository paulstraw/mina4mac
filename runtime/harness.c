// Differential-test harness: load a memory/register snapshot, run one recompiled function,
// write back the resulting registers and memory regions.
//   harness <snapshot-in> <result-out> [<image.bin> <base-hex>]
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

#include "cpu.h"

typedef struct { uint32_t addr; GuestFn fn; } FnEntry;
extern const FnEntry FN_TABLE[];
extern const int FN_COUNT;

uint8_t *MEM;

static GuestFn lookup(uint32_t a) {
    int lo = 0, hi = FN_COUNT - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (FN_TABLE[mid].addr == a) return FN_TABLE[mid].fn;
        if (FN_TABLE[mid].addr < a) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}

void guest_call(CPU *c, uint32_t target) {
    GuestFn f = lookup(target);
    if (!f) { fprintf(stderr, "no function at %#x\n", target); exit(5); }
    f(c);
}
void guest_import(CPU *c, uint32_t slot) { (void)c; fprintf(stderr, "import %#x\n", slot); exit(4); }
void guest_unimpl(CPU *c, uint32_t addr, const char *what) {
    (void)c; fprintf(stderr, "unimpl %#x %s\n", addr, what); exit(3);
}

typedef struct { uint32_t addr, len; } Region;

int main(int argc, char **argv) {
    if (argc != 3 && argc != 5) return 2;
    MEM = mmap(NULL, 1ull << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (MEM == MAP_FAILED) { perror("mmap"); return 2; }
    if (argc == 5) {
        FILE *im = fopen(argv[3], "rb");
        uint32_t base = (uint32_t)strtoul(argv[4], NULL, 16);
        fread(MEM + base, 1, 64 << 20, im);
        fclose(im);
    }
    FILE *f = fopen(argv[1], "rb");
    uint32_t func, nreg;
    CPU c = {0};
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
    GuestFn fn = lookup(func);
    if (!fn) { fprintf(stderr, "no function %#x\n", func); return 5; }
    fn(&c);
    FILE *o = fopen(argv[2], "wb");
    fwrite(&c.eax, 4, 9, o);
    fwrite(c.xmm, 16, 8, o);
    for (uint32_t i = 0; i < nreg; i++) fwrite(MEM + regs[i].addr, 1, regs[i].len, o);
    fclose(o);
    return 0;
}
