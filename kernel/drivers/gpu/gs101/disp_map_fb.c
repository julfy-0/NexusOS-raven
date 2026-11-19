#include "a64.h"
#include "gs101_disp.h"

/* Мапим буфер ABL как Normal non-cacheable (записи видны DPU без clean кэша).
 * Верно только если SysMMU DPU выключены (адрес физический) — см. s->sysmmu_on. */
int disp_map_fb(struct disp_state *s) {
    if (s->ch < 0 || s->width == 0 || s->height == 0 || s->width > 4096 || s->height > 8192) return -1;
    uint64_t bytes = (uint64_t)s->pitch * s->height;
    uint64_t a = s->fb_phys & ~0x1FFFFFULL;
    uint64_t end = s->fb_phys + bytes;
    for (; a < end; a += 0x200000ULL) mmu_map_2m(a, MMU_ATTR_NC);
    __asm__ volatile("dsb ishst; tlbi vmalle1; dsb nsh; isb");
    s->fb = (volatile uint32_t *)(uintptr_t)s->fb_phys;
    return 0;
}
