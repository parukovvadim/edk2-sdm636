#include <AsmMacroIoLibV8.h>

.data
.align 3

GCC_ASM_EXPORT (gXnuEntryPa)
.global gXnuEntryPa
gXnuEntryPa:
    .quad 0

.text
.align 3

GCC_ASM_EXPORT (gold_entry)

gold_entry:
    // PSCI CPU_ON enters with x0 = context_id = ba_pa
    mov     x19, x0

    // Ensure all interrupts are masked
    msr     daifset, #0xf

    // Invalidate Instruction Cache and barrier
    dsb     sy
    ic      iallu
    dsb     sy
    isb

    // Load XNU physical entry point from gXnuEntryPa
    adrp    x1, gXnuEntryPa
    ldr     x1, [x1, :lo12:gXnuEntryPa]

    // x0 = physical address of boot_args structure
    mov     x0, x19

    // Transfer execution to Apple XNU kernel!
    br      x1

dead:
    wfe
    b       dead
