// KERNEL32 HLE pieces other HLE files share (kernel32.c).
#pragma once
#include <stdint.h>

// Initialise the guest CRITICAL_SECTION at `cs` (InitializeCriticalSection and friends).
void cs_init(uint32_t cs);

// Kernel object handles: small guest values (4-aligned, like Windows') naming a host-side object.
// The pseudo-handles GetCurrentProcess/GetCurrentThread return are not in the table.
enum { H_CURRENT_PROCESS = 0xffffffffu, H_CURRENT_THREAD = 0xfffffffeu };
enum HandleKind { HK_FREE, HK_THREAD, HK_PROCESS, HK_FIND };
uint32_t handle_new(enum HandleKind kind, uint64_t data);  // 0 if the table is full
int handle_get(uint32_t h, enum HandleKind kind, uint64_t *data);  // 0 if h isn't a live `kind` handle
int handle_close(uint32_t h);  // free h's table entry (not its host object); 0 if h isn't live
