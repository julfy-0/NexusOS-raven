#include "a64.h"
#include "psci_diag.h"

struct smc_ret { uint64_t x0, x1, x2, x3; };

static struct smc_ret smc_call(uint64_t id, uint64_t a1, uint64_t a2, uint64_t a3) {
    register uint64_t r0 __asm__("x0") = id;
    register uint64_t r1 __asm__("x1") = a1;
    register uint64_t r2 __asm__("x2") = a2;
    register uint64_t r3 __asm__("x3") = a3;
    __asm__ volatile("smc #0" : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3)
                     :: "x4","x5","x6","x7","x8","x9","x10","x11","x12",
                        "x13","x14","x15","x16","x17","memory");
    struct smc_ret r = { r0, r1, r2, r3 };
    return r;
}

static void log_call(const char *name, uint64_t id, uint64_t a1) {
    struct smc_ret r = smc_call(id, a1, 0, 0);
    ramoops_puts(name);
    ramoops_puts(" x0="); ramoops_put_hex(r.x0);
    ramoops_puts(" x1="); ramoops_put_hex(r.x1);
    ramoops_puts(" x2="); ramoops_put_hex(r.x2);
    ramoops_puts(" x3="); ramoops_put_hex(r.x3);
    ramoops_puts("\n");
}

/* ID функций взяты из спецификаций SMCCC/PSCI, а не из ~/gs. */
void psci_diag(void) {
    uint64_t el;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
    ramoops_puts("CurrentEL="); ramoops_put_hex(el >> 2); ramoops_puts("\n");
    log_call("SMCCC_VERSION    ", 0x80000000ULL, 0);
    log_call("PSCI_VERSION     ", 0x84000000ULL, 0);
    log_call("FEAT SYSTEM_OFF  ", 0x8400000AULL, 0x84000008ULL);
    log_call("FEAT SYSTEM_RESET", 0x8400000AULL, 0x84000009ULL);
    log_call("FEAT RESET2_32   ", 0x8400000AULL, 0x84000012ULL);
    log_call("FEAT RESET2_64   ", 0x8400000AULL, 0xC4000012ULL);
    ramoops_flush();
}
