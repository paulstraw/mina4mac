// Import thunk test (built and run by tools/importtest.py). Binds noita.exe's IAT to host thunks, then
// runs lifted `call [slot]` / `jmp [slot]` instructions against fake host implementations.
//   import_test <image.bin>            run the checks
//   import_test <image.bin> unimpl     call an import with no implementation (must exit 4)
#include <stdio.h>
#include <string.h>

#include "rt.h"

extern const int N_IMPORTS;
extern const char *CALL_DLL, *CALL_NAME, *JMP_DLL, *JMP_NAME, *UNIMPL_DLL, *UNIMPL_NAME;
extern const uint32_t CALL_SLOT, CALL_RET, JMP_SLOT, JMP_RET, UNIMPL_SLOT, UNIMPL_RET;
void T_call(CPU *c);    // call [GetCurrentProcessId slot]; ret
void T_jmp(CPU *c);     // jmp [QueryPerformanceCounter slot]
void T_unimpl(CPU *c);  // call [GetCurrentThreadId slot]; ret

enum { STACK_TOP = 0x100000, CALLER_RET = 0x0badf000 };
static uint32_t seen_ret, seen_arg;
static int calls;

// Fake KERNEL32!GetCurrentProcessId: stdcall, no args.
static void fake_pid(CPU *c) {
    calls++;
    seen_ret = rd32(c->esp);
    c->eax = 0x12345678;
    c->esp += 4;
}
// Fake KERNEL32!QueryPerformanceCounter: stdcall, one arg.
static void fake_sleep(CPU *c) {
    calls++;
    seen_ret = rd32(c->esp);
    seen_arg = rd32(c->esp + 4);
    c->eax = 0xdead;
    c->esp += 8;
}

static int fails;
#define CHECK(name, got, want)                                                                        \
    do {                                                                                              \
        uint32_t g_ = (got), w_ = (want);                                                             \
        printf("%-22s %s got %#x want %#x\n", name, g_ == w_ ? "ok  " : "FAIL", g_, w_);             \
        fails += g_ != w_;                                                                            \
    } while (0)

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    rt_init();
    rt_load_image(argv[1], 0x400000);
    rt_register_import("kernel32.dll", "GetCurrentProcessId", fake_pid);  // dll name is case-insensitive
    CHECK("slots bound", rt_bind_imports(0x400000), N_IMPORTS);
    rt_register_import(JMP_DLL, JMP_NAME, fake_sleep);  // registering after binding works too
    CPU c = {.fpu_cw = 0x27f};

    if (argc > 2) {  // unimpl
        c.esp = STACK_TOP;
        T_unimpl(&c);
        return 0;
    }

    uint32_t t = rd32(CALL_SLOT);
    CHECK("thunk in range", t >= THUNK_BASE && (t - THUNK_BASE) % THUNK_STRIDE == 0, 1);
    CHECK("thunk dedup", rt_thunk("KERNEL32.DLL", "GetCurrentProcessId"), t);
    CHECK("thunks distinct", rd32(JMP_SLOT) != t && rd32(UNIMPL_SLOT) != t, 1);

    c.esp = STACK_TOP;
    T_call(&c);  // call [slot]; ret: pops the test's own return address too
    CHECK("call: host ran", calls, 1);
    CHECK("call: return addr", seen_ret, CALL_RET);
    CHECK("call: eax", c.eax, 0x12345678);
    CHECK("call: esp", c.esp, STACK_TOP + 4);

    // Tail jump: the host function sees the caller's return address and argument.
    c.esp = STACK_TOP - 8;
    wr32(c.esp, CALLER_RET);
    wr32(c.esp + 4, 250);
    T_jmp(&c);
    CHECK("jmp: host ran", calls, 2);
    CHECK("jmp: return addr", seen_ret, CALLER_RET);
    CHECK("jmp: arg", seen_arg, 250);
    CHECK("jmp: eax", c.eax, 0xdead);
    CHECK("jmp: esp", c.esp, STACK_TOP);

    // guest_call on a thunk address directly (e.g. a function pointer taken from the IAT).
    c.esp = STACK_TOP - 4;
    wr32(c.esp, CALLER_RET);
    guest_call(&c, t);
    CHECK("guest_call thunk", calls == 3 && c.esp == STACK_TOP && seen_ret == CALLER_RET, 1);
    return fails != 0;
}
