// Guest CPU state and memory model shared by recompiled code and the runtime.
#pragma once
#include <stdint.h>
#include <string.h>

typedef union {
    uint8_t b[16];
    uint32_t u32[4];
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
} CPU;

// Guest memory: a 4 GB region; guest address a lives at MEM + a.
extern uint8_t *MEM;

typedef uint32_t __attribute__((aligned(1), may_alias)) u32u;
typedef uint16_t __attribute__((aligned(1), may_alias)) u16u;
typedef uint64_t __attribute__((aligned(1), may_alias)) u64u;
typedef float __attribute__((aligned(1), may_alias)) f32u;
typedef double __attribute__((aligned(1), may_alias)) f64u;

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

typedef void (*GuestFn)(CPU *);
void guest_call(CPU *c, uint32_t target);   // indirect call dispatch (runtime)
void guest_import(CPU *c, uint32_t slot);   // call through an IAT slot (runtime)
void guest_unimpl(CPU *c, uint32_t addr, const char *what);
