#include "a64.h"

extern uint64_t mmu_l1[512];

void mmu_init(uint64_t load_base) {
    /* 0..1 ГиБ — MMIO (UART 0x10A00000, GIC 0x10400000, UFS 0x14700000, DPU 0x1C300000) */
    mmu_l1[0] = 0x0ULL | (1ULL << 10) | (2ULL << 8) | ((uint64_t)MMU_ATTR_DEVICE << 2) |
                (1ULL << 53) | (1ULL << 54) | 1;

    /* окно вокруг образа ядра: 8 МиБ, Normal WB */
    uint64_t b = load_base & ~0x1FFFFFULL;
    for (int i = 0; i < 4; i++) mmu_map_2m(b + ((uint64_t)i << 21), MMU_ATTR_WB);

    /* ramoops (4 МиБ с 0xFD3FF000) — Normal non-cacheable */
    for (uint64_t a = RAMOOPS_BASE & ~0x1FFFFFULL; a < RAMOOPS_BASE + 0x400000ULL; a += 0x200000ULL)
        mmu_map_2m(a, MMU_ATTR_NC);

    uint64_t mair = (0x00ULL << 0) | (0x44ULL << 8) | (0xFFULL << 16);
    uint64_t mmfr0;
    __asm__ volatile("mrs %0, id_aa64mmfr0_el1" : "=r"(mmfr0));
    uint64_t ips = mmfr0 & 0xF; if (ips > 5) ips = 5;
    uint64_t tcr = 25ULL | (1ULL << 8) | (1ULL << 10) | (3ULL << 12) | (0ULL << 14) |
                   (1ULL << 23) | (ips << 32);
    uint64_t sctlr = 0x30d00800ULL | 1ULL | (1ULL << 2) | (1ULL << 12);

    __asm__ volatile(
        "dsb sy\n"
        "msr mair_el1, %0\n"
        "msr tcr_el1, %1\n"
        "msr ttbr0_el1, %2\n"
        "isb\n"
        "tlbi vmalle1\n"
        "dsb nsh\n"
        "isb\n"
        "ic iallu\n"
        "dsb nsh\n"
        "isb\n"
        "msr sctlr_el1, %3\n"
        "isb\n"
        :: "r"(mair), "r"(tcr), "r"((uint64_t)(uintptr_t)mmu_l1), "r"(sctlr) : "memory");
}
