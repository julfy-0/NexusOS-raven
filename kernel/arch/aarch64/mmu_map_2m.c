#include "a64.h"

/* Три уровня не нужны: VA 39 бит, старт с L1 (блоки 1 ГиБ), L2 — блоки 2 МиБ. */
uint64_t mmu_l1[512] __attribute__((aligned(4096)));
uint64_t mmu_l2[6][512] __attribute__((aligned(4096)));
int      mmu_l2_used;
int      mmu_l2_idx_of_l1[512];

void mmu_map_2m(uint64_t phys, int attr) {
    uint64_t gb = phys >> 30;
    if (gb >= 512) return;
    if ((mmu_l1[gb] & 1) && !(mmu_l1[gb] & 2)) return;   /* уже блок 1 ГиБ */
    if (!(mmu_l1[gb] & 1)) {
        if (mmu_l2_used >= 6) return;
        int t = mmu_l2_used++;
        mmu_l2_idx_of_l1[gb] = t;
        mmu_l1[gb] = (uint64_t)(uintptr_t)mmu_l2[t] | 3;   /* table */
    }
    uint64_t *l2 = (uint64_t *)(uintptr_t)(mmu_l1[gb] & ~0xFFFULL);
    uint64_t idx = (phys >> 21) & 511;
    uint64_t sh = (attr == MMU_ATTR_DEVICE) ? 2 : 3;        /* outer / inner */
    uint64_t xn = (attr == MMU_ATTR_WB) ? 0 : ((1ULL << 53) | (1ULL << 54));
    l2[idx] = (phys & ~0x1FFFFFULL) | (1ULL << 10) | (sh << 8) |
              ((uint64_t)attr << 2) | xn | 1;
}
