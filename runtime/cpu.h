// Guest CPU state and memory model shared by recompiled code and the runtime.
#pragma once
#include <stdint.h>
#include <string.h>

typedef union {
    uint8_t b[16];
    int8_t i8[16];
    uint16_t u16[8];
    int16_t i16[8];
    uint32_t u32[4];
    int32_t i32[4];
    uint64_t u64[2];
    float f32[4];
    double f64[2];
} Xmm;

typedef struct CPU {
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t fs_base;  // guest address of the TEB
    Xmm xmm[8];
    double st[8];      // x87 stack, modelled in double precision
    int st_top;
    uint16_t fpu_cw;   // x87 control word (rounding mode used by fist/fistp)
    uint16_t fpu_sw;   // x87 condition bits C0/C2/C3 from fcom*
} CPU;

// Guest memory: a 4 GB region; guest address a lives at MEM + a.
extern uint8_t *MEM;

typedef uint32_t __attribute__((aligned(1), may_alias)) u32u;
typedef uint16_t __attribute__((aligned(1), may_alias)) u16u;
typedef uint64_t __attribute__((aligned(1), may_alias)) u64u;
typedef float __attribute__((aligned(1), may_alias)) f32u;
typedef double __attribute__((aligned(1), may_alias)) f64u;

// Segment selector values as seen by a 32-bit process on 64-bit Windows (WoW64).
#define SEL_CS 0x23
#define SEL_DS 0x2b
#define SEL_FS 0x53

#define P(a) (MEM + (uint32_t)(a))
static inline uint8_t rd8(uint32_t a) { return *P(a); }
static inline uint16_t rd16(uint32_t a) { return *(u16u *)P(a); }
static inline uint32_t rd32(uint32_t a) { return *(u32u *)P(a); }
static inline uint64_t rd64(uint32_t a) { return *(u64u *)P(a); }
static inline void wr8(uint32_t a, uint8_t v) { *P(a) = v; }
static inline void wr16(uint32_t a, uint16_t v) { *(u16u *)P(a) = v; }
static inline void wr32(uint32_t a, uint32_t v) { *(u32u *)P(a) = v; }
static inline void wr64(uint32_t a, uint64_t v) { *(u64u *)P(a) = v; }
static inline float rdf32(uint32_t a) { return *(f32u *)P(a); }
static inline double rdf64(uint32_t a) { return *(f64u *)P(a); }
static inline void wrf32(uint32_t a, float v) { *(f32u *)P(a) = v; }
static inline void wrf64(uint32_t a, double v) { *(f64u *)P(a) = v; }

static inline int16_t sat16(int32_t v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v; }
static inline uint8_t usat8(int16_t v) { return v > 255 ? 255 : v < 0 ? 0 : (uint8_t)v; }

static inline int parity8(uint32_t v) { return !__builtin_parity(v & 0xff); }

// x86 cvtt*2si semantics: out-of-range / NaN produce 0x80000000.
static inline uint32_t cvtt_f32_i32(float f) {
    return (f != f || f >= 2147483648.0f || f < -2147483648.0f) ? 0x80000000u : (uint32_t)(int32_t)f;
}
static inline uint32_t cvtt_f64_i32(double f) {
    return (f != f || f >= 2147483648.0 || f < -2147483649.0) ? 0x80000000u : (uint32_t)(int32_t)f;
}
static inline float f32_from_bits(uint32_t v) { float f; memcpy(&f, &v, 4); return f; }
static inline uint32_t f32_bits(float f) { uint32_t v; memcpy(&v, &f, 4); return v; }

#define ST(i) c->st[(c->st_top + (i)) & 7]
static inline void st_push(CPU *c, double v) { c->st_top = (c->st_top - 1) & 7; c->st[c->st_top] = v; }
static inline void st_pop(CPU *c) { c->st_top = (c->st_top + 1) & 7; }
static inline double fpu_round(CPU *c, double v) {
    switch ((c->fpu_cw >> 10) & 3) {
    case 0: return __builtin_rint(v);
    case 1: return __builtin_floor(v);
    case 2: return __builtin_ceil(v);
    default: return __builtin_trunc(v);
    }
}
// x87 float->int stores: out-of-range / NaN give the "integer indefinite" value.
static inline int64_t fist64(double v, int bits) {
    double lim = bits == 64 ? 9223372036854775808.0 : bits == 32 ? 2147483648.0 : 32768.0;
    if (v != v || v >= lim || v < -lim) return bits == 64 ? INT64_MIN : bits == 32 ? INT32_MIN : INT16_MIN;
    return (int64_t)v;
}
static inline uint16_t fcom_bits(double a, double b) {
    if (a != a || b != b) return 0x4500;
    return a < b ? 0x0100 : a == b ? 0x4000 : 0;
}

// punpck{l,h}{bw,wd,dq,qdq}: interleave the low (hi=0) or high (hi=1) halves of a and b,
// element size esz bytes.
static inline Xmm xmm_unpack(Xmm a, Xmm b, int esz, int hi) {
    Xmm r;
    int off = hi ? 8 : 0;
    for (int k = 0; k < 8 / esz; k++) {
        memcpy(&r.b[2 * k * esz], &a.b[off + k * esz], esz);
        memcpy(&r.b[(2 * k + 1) * esz], &b.b[off + k * esz], esz);
    }
    return r;
}

// cpuid with fixed answers: a generic SSE2-capable Intel CPU (family 6, "GenuineIntel", max leaf 1).
// tools/difftest.py hooks Unicorn's cpuid with the same values.
static inline void cpuid_fixed(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    switch (leaf) {
    case 0: *a = 1; *b = 0x756e6547; *d = 0x49656e69; *c = 0x6c65746e; break;
    // FPU TSC CX8 CMOV MMX FXSR SSE SSE2; 64-byte clflush line.
    case 1: *a = 0x000006f6; *b = 0x00000800; *c = 0; *d = 0x07808111; break;
    default: *a = *b = *c = *d = 0; break;
    }
}

typedef void (*GuestFn)(CPU *);
void guest_call(CPU *c, uint32_t target);   // indirect call dispatch (runtime)
void guest_import(CPU *c, uint32_t slot);   // call through an IAT slot (runtime)
void guest_unimpl(CPU *c, uint32_t addr, const char *what);
