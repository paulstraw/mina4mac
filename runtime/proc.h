// Guest process environment: the PEB, and per-thread guest stacks and TEBs. See runtime/README.md for
// the memory map.
#pragma once
#include "cpu.h"

#define STACKS_LO 0x1A000000u   // thread slot n: a PROT_NONE guard, then its stack
#define STACK_GUARD 0x10000u
#define STACK_SIZE 0x100000u    // 1 MB, noita.exe's SizeOfStackReserve
#define STACK_SLOT (STACK_GUARD + STACK_SIZE)
#define TEBS_LO 0x1F000000u     // thread slot n: TEB page, then its TLS pointer array page
#define TEB_SLOT 0x2000u
#define MAX_THREADS 64
#define PEB_ADDR 0x1F800000u
#define PROCESS_HEAP 0x1F810000u  // handle returned by GetProcessHeap; nothing lives there
#define GUEST_PID 0x100u          // guest thread n has id THREAD_TID(n)
#define THREAD_TID(n) (GUEST_PID + 4 * ((uint32_t)(n) + 1))
#define THREAD_SLOT(tid) ((int)(((tid) - GUEST_PID) / 4) - 1)

// TEB fields (x86), read by guest code as fs:[offset].
enum {
    TEB_EXCEPTION_LIST = 0x00, TEB_STACK_BASE = 0x04, TEB_STACK_LIMIT = 0x08, TEB_SELF = 0x18,
    TEB_PID = 0x20, TEB_TID = 0x24, TEB_TLS_POINTER = 0x2c, TEB_PEB = 0x30, TEB_LAST_ERROR = 0x34,
    TEB_TLS_SLOTS = 0xe10,  // TlsAlloc slots 0..63
    TEB_CRT_ERRNO = 0xff0,  // HLE msvcr120's per-thread errno (a spare TEB field)
};
// PEB fields.
enum { PEB_BEING_DEBUGGED = 0x02, PEB_IMAGE_BASE = 0x08, PEB_PROCESS_HEAP = 0x18,
       PEB_OS_MAJOR = 0xa4, PEB_OS_MINOR = 0xa8, PEB_OS_BUILD = 0xac, PEB_OS_PLATFORM = 0xb0 };

// Set up the PEB for the exe mapped at guest `exe_base` (headers included) and read its static TLS
// template (IMAGE_TLS_DIRECTORY), writing TLS index 0 to its AddressOfIndex. TLS callbacks are not run.
void rt_process_init(uint32_t exe_base);

// Give the calling host thread a guest thread: a stack slot and a TEB with an empty SEH chain, stack
// bounds, ids, the PEB and a fresh copy of the static TLS block (from the guest heap) at TLS index 0.
// Sets c->esp to the stack top and c->fs_base to the TEB, and returns the TEB address.
uint32_t rt_thread_init(CPU *c);
// The same in two steps, for a thread whose id is needed before it runs: reserve slot n (its id is
// THREAD_TID(n)), then set it up on the thread itself.
int rt_thread_reserve(void);
uint32_t rt_thread_setup(CPU *c, int n);
