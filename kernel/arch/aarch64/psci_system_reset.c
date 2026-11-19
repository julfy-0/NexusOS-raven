#include "a64.h"
#include "pmu_smc.h"

/* Сначала PMU swreset: SYSTEM_CONFIGURATION PMU+0x3A00 <- 0x2 (gs101.dtsi syscon-reboot; в
 * mainline это обычная запись, не atomic). Запись идёт через SMC (pmu_smc.c): прямая запись
 * с EL1 в PMU_ALIVE, по mainline, не допускается — предположение, не проверено на железе.
 * Если не сработало — PSCI SYSTEM_RESET (0x84000009). */
void psci_system_reset(void) {
    uint64_t rc = pmu_smc_write(0x3A00, 0x2U);
    ramoops_puts("[reset] pmu_smc swreset rc="); ramoops_put_hex(rc); ramoops_puts("\n");
    ramoops_flush();
    __asm__ volatile("dsb sy" ::: "memory");
    for (volatile int i = 0; i < 20000000; i++) { }

    register uint64_t x0 __asm__("x0") = 0x84000009ULL;
    __asm__ volatile("smc #0" : "+r"(x0) :: "x1", "x2", "x3", "memory");
    ramoops_puts("[reset] PSCI SYSTEM_RESET returned x0="); ramoops_put_hex(x0); ramoops_puts("\n");
    ramoops_flush();
    for (;;) __asm__ volatile("wfe");
}
