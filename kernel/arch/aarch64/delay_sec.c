#include "a64.h"

void delay_sec(uint32_t s) {
    uint64_t f = timer_freq(), t0, t;
    if (!f) f = 24576000ULL;                 /* gs101.dtsi: clock-frequency */
    __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(t0));
    do { __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(t)); } while (t - t0 < f * s);
}

void delay_ms(uint32_t ms) {
    uint64_t f = timer_freq(), t0, t;
    if (!f) f = 24576000ULL;                 /* gs101.dtsi: clock-frequency */
    uint64_t ticks = (f * (uint64_t)ms) / 1000ULL;
    __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(t0));
    do { __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(t)); } while (t - t0 < ticks);
}
