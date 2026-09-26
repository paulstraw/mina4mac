// KERNEL32 HLE pieces other HLE files share (kernel32.c).
#pragma once
#include <stdint.h>

// Initialise the guest CRITICAL_SECTION at `cs` (InitializeCriticalSection and friends).
void cs_init(uint32_t cs);
