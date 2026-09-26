// Host function registry test (built and run by tools/check.sh). Calls host implementations declared
// with host.h through thunks the way guest code does (push args, push return address, guest_call) and
// checks arguments, return values and stack cleanup for cdecl and stdcall.
#include <stdio.h>
#include <string.h>

#include "host.h"

enum { STACK_TOP = 0x100000, RET_ADDR = 0x0badf000, DATA = 0x200000, GUEST_DOUBLE = 0x401000 };

// A "recompiled" guest function for host->guest callbacks: cdecl int f(int x) { return 2 * x; }
static void F_guest_double(CPU *c) {
    c->eax = 2 * rd32(c->esp + 4);
    c->esp += 4;
}
const FnEntry FN_TABLE[] = {{GUEST_DOUBLE, F_guest_double}};
const int FN_COUNT = 1;

HOST_CDECL(test, sub3) { ret_i32(c, ARG(0) - ARG(1) - ARG(2)); }
HOST_STDCALL(test, sub3_std, 12) { ret_i32(c, ARG(0) - ARG(1) - ARG(2)); }
HOST_CDECL(test, mul64) { ret_i64(c, ARG_I64(0) * ARG(2)); }
HOST_CDECL(test, scale) { ret_f64(c, ARG_F64(0) * ARG_F32(2)); }        // double scale(double, float)
HOST_STDCALL(test, halve_f, 4) { ret_f32(c, ARG_F32(0) / 2); }          // float __stdcall halve_f(float)
HOST_CDECL(test, sse2_neg) { ret_xmm0_f64(c, -ARG_F64(0)); }
HOST_CDECL(test, strlen_or_null) { ret_i32(c, ARG_STR(0) ? strlen(ARG_STR(0)) : 0xffffffffu); }
HOST(test, op_delete, "??3@YAXPAX@Z", 0) { ret_i32(c, ARG(0) + 1); }
// Calls back into guest code, leaving esp moved; the wrapper still cleans up per convention.
HOST_STDCALL(test, callback, 8) {
    c->esp -= 8;
    wr32(c->esp + 4, ARG(1));
    wr32(c->esp, RET_ADDR + 1);
    guest_call(c, ARG(0));
    ret_i32(c, c->eax + 1);
}

// Guest-side call of test!name with 4-byte args; returns esp after the call (before any caller cleanup).
static uint32_t call(CPU *c, const char *name, int n, const uint32_t *args) {
    c->esp = STACK_TOP;
    for (int i = n - 1; i >= 0; i--) { c->esp -= 4; wr32(c->esp, args[i]); }
    c->esp -= 4;
    wr32(c->esp, RET_ADDR);
    guest_call(c, rt_thunk("TEST.DLL", name));
    return c->esp;
}

static int fails;
#define CHECK(name, got, want)                                                                        \
    do {                                                                                              \
        uint64_t g_ = (got), w_ = (want);                                                             \
        printf("%-24s %s got %#llx want %#llx\n", name, g_ == w_ ? "ok  " : "FAIL", g_, w_);         \
        fails += g_ != w_;                                                                            \
    } while (0)

static uint32_t dbl_lo(double d) { uint64_t v; memcpy(&v, &d, 8); return (uint32_t)v; }
static uint32_t dbl_hi(double d) { uint64_t v; memcpy(&v, &d, 8); return v >> 32; }

int main(void) {
    rt_init();
    CPU c = {.fpu_cw = 0x27f};

    CHECK("cdecl: eax", (call(&c, "sub3", 3, (uint32_t[]){100, 30, 7}), c.eax), 63);
    CHECK("cdecl: esp", c.esp, STACK_TOP - 12);  // caller pops the args
    CHECK("stdcall: eax", (call(&c, "sub3_std", 3, (uint32_t[]){100, 30, 7}), c.eax), 63);
    CHECK("stdcall: esp", c.esp, STACK_TOP);     // callee popped the args

    call(&c, "mul64", 3, (uint32_t[]){0x89abcdef, 0x01234567, 16});
    CHECK("i64: edx:eax", (uint64_t)c.edx << 32 | c.eax, 0x0123456789abcdefull * 16);

    call(&c, "scale", 3, (uint32_t[]){dbl_lo(1.5), dbl_hi(1.5), f32_bits(-4.0f)});
    CHECK("f64 st0: pushed", c.st_top, 7);
    CHECK("f64 st0: value", c.st[c.st_top] == -6.0, 1);
    CHECK("f64 st0: esp", c.esp, STACK_TOP - 12);
    st_pop(&c);

    call(&c, "halve_f", 1, (uint32_t[]){f32_bits(5.0f)});
    CHECK("f32 st0: value", c.st_top == 7 && c.st[c.st_top] == 2.5, 1);
    CHECK("f32 st0: esp", c.esp, STACK_TOP);
    st_pop(&c);

    call(&c, "sse2_neg", 2, (uint32_t[]){dbl_lo(0.25), dbl_hi(0.25)});
    CHECK("xmm0: value", c.xmm[0].f64[0] == -0.25 && c.xmm[0].f64[1] == 0 && c.st_top == 0, 1);

    strcpy((char *)P(DATA), "noita");
    CHECK("ARG_STR", (call(&c, "strlen_or_null", 1, (uint32_t[]){DATA}), c.eax), 5);
    CHECK("ARG_PTR null", (call(&c, "strlen_or_null", 1, (uint32_t[]){0}), c.eax), 0xffffffff);

    CHECK("mangled name", (call(&c, "??3@YAXPAX@Z", 1, (uint32_t[]){41}), c.eax), 42);

    CHECK("guest callback: eax", (call(&c, "callback", 2, (uint32_t[]){GUEST_DOUBLE, 20}), c.eax), 41);
    CHECK("guest callback: esp", c.esp, STACK_TOP);
    return fails != 0;
}
