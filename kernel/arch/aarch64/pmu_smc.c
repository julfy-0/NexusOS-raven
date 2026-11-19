#include "pmu_smc.h"

#define TENSOR_SMC_PMU_SEC_REG 0x82000504ULL
#define TENSOR_PMUREG_WRITE    1ULL
#define TENSOR_PMUREG_RMW      2ULL
#define TENSOR_SET_BITS        0xC000U   /* BIT(15)|BIT(14) */
#define TENSOR_CLR_BITS        0x8000U   /* BIT(15) */

static inline uint64_t smc5(uint64_t fid, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    register uint64_t x0 __asm__("x0") = fid;
    register uint64_t x1 __asm__("x1") = a1;
    register uint64_t x2 __asm__("x2") = a2;
    register uint64_t x3 __asm__("x3") = a3;
    register uint64_t x4 __asm__("x4") = a4;
    __asm__ volatile("smc #0"
                     : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4)
                     :
                     : "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13",
                       "x14", "x15", "x16", "x17", "memory");
    return x0;
}

uint64_t pmu_smc_write(uint32_t off, uint32_t val) {
    return smc5(TENSOR_SMC_PMU_SEC_REG, PMU_SMC_BASE + off, TENSOR_PMUREG_WRITE, val, 0);
}

uint64_t pmu_smc_rmw(uint32_t off, uint32_t mask, uint32_t val) {
    return smc5(TENSOR_SMC_PMU_SEC_REG, PMU_SMC_BASE + off, TENSOR_PMUREG_RMW, mask, val);
}

uint64_t pmu_smc_set_bit(uint32_t off, uint32_t bit) {
    return pmu_smc_write(off | TENSOR_SET_BITS, bit);
}

uint64_t pmu_smc_clear_bit(uint32_t off, uint32_t bit) {
    return pmu_smc_write(off | TENSOR_CLR_BITS, bit);
}
