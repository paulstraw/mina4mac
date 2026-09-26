// Host implementations of imported functions, declared by dll!name with their calling convention.
//
//   HOST_CDECL(msvcr120, abs) { ret_i32(c, abs((int32_t)ARG(0))); }
//   HOST_STDCALL(kernel32, Sleep, 4) { usleep(ARG(0) * 1000); }
//   HOST(msvcp120, op_delete, "??3@YAXPAX@Z", 0) { ... }   // names that aren't C identifiers
//
// Each declaration registers itself with rt_register_import at startup (a constructor), so linking the
// file is enough. The dll token gets ".dll" appended and is matched case-insensitively. The body runs
// with `CPU *c` and `uint32_t argp`, the guest address of the first stack argument. After the body,
// esp is set to pop the return address plus `argbytes` (0 for cdecl, the stdcall @N for stdcall), so
// the body may push and call back into guest code freely. Registers the body doesn't set keep their
// values, which is fine: eax/ecx/edx and the x87 stack are caller-saved.
#pragma once
#include "rt.h"

#define HOST(dll, id, name, argbytes)                                                               \
    static void host_##dll##_##id(CPU *c, uint32_t argp);                                            \
    static void hostcall_##dll##_##id(CPU *c) {                                                      \
        uint32_t sp = c->esp;                                                                        \
        host_##dll##_##id(c, sp + 4);                                                                \
        c->esp = sp + 4 + (argbytes);                                                                \
    }                                                                                                \
    __attribute__((constructor)) static void hostreg_##dll##_##id(void) {                            \
        rt_register_import(#dll ".dll", name, hostcall_##dll##_##id);                                \
    }                                                                                                \
    static void host_##dll##_##id(__attribute__((unused)) CPU *c, __attribute__((unused)) uint32_t argp)
#define HOST_CDECL(dll, name) HOST(dll, name, #name, 0)
#define HOST_STDCALL(dll, name, argbytes) HOST(dll, name, #name, argbytes)

// Stack arguments, indexed in 4-byte slots (a double or 64-bit integer takes two).
#define ARG_ADDR(n) (argp + 4 * (n))
#define ARG(n) rd32(ARG_ADDR(n))
#define ARG_I64(n) rd64(ARG_ADDR(n))
#define ARG_F32(n) rdf32(ARG_ADDR(n))
#define ARG_F64(n) rdf64(ARG_ADDR(n))
#define ARG_PTR(n) host_ptr(ARG(n))                // guest pointer as a host pointer (NULL stays NULL)
#define ARG_STR(n) ((const char *)ARG_PTR(n))

static inline void *host_ptr(uint32_t a) { return a ? P(a) : NULL; }

// Return values: integers and pointers in eax, 64-bit integers in edx:eax, float and double in x87
// st0 (pushed; the caller pops it), and SSE2 CRT variants (__libm_sse2_*) in xmm0.
static inline void ret_i32(CPU *c, uint32_t v) { c->eax = v; }
static inline void ret_i64(CPU *c, uint64_t v) { c->eax = (uint32_t)v; c->edx = (uint32_t)(v >> 32); }
static inline void ret_f64(CPU *c, double v) { st_push(c, v); }
static inline void ret_f32(CPU *c, float v) { st_push(c, v); }
static inline void ret_xmm0_f64(CPU *c, double v) { c->xmm[0] = (Xmm){.f64 = {v, 0}}; }
static inline void ret_xmm0_f32(CPU *c, float v) { c->xmm[0] = (Xmm){.f32 = {v, 0, 0, 0}}; }
