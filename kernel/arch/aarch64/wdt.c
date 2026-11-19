#include "a64.h"
#include "wdt.h"

/* Базовые адреса аппаратных таймеров Watchdog для кластеров Google Tensor GS101 */
#define WDT_CL0_BASE  0x10060000ULL
#define WDT_CL1_BASE  0x10070000ULL

/* Смещения регистров Samsung / Exynos S3C2410-совместимого WDT */
#define WTCON_OFFSET    0x00
#define WTDAT_OFFSET    0x04
#define WTCNT_OFFSET    0x08
#define WTCLRINT_OFFSET 0x0C

/* Контроллер питания PMU */
#define PMU_BASE              0x17460000ULL
#define PMU_CL0_NONCPU_INT_EN 0x1244   /* s3c2410_wdt.c:78 */
#define PMU_CL1_NONCPU_INT_EN 0x1444   /* s3c2410_wdt.c:79 */

static inline uint32_t wdt_read(uint64_t base, uint32_t off) {
    return *(volatile uint32_t *)(uintptr_t)(base + off);
}

static inline void wdt_write(uint64_t base, uint32_t off, uint32_t val) {
    *(volatile uint32_t *)(uintptr_t)(base + off) = val;
}

static void disable_cluster_wdt(uint64_t base, const char *name) {
    uint32_t con = wdt_read(base, WTCON_OFFSET);
    uint32_t cnt = wdt_read(base, WTCNT_OFFSET);

    ramoops_puts("[wdt] ");
    ramoops_puts(name);
    ramoops_puts(" pre-disable WTCON=");
    ramoops_put_hex((uint64_t)con);
    ramoops_puts(" WTCNT=");
    ramoops_put_hex((uint64_t)cnt);
    ramoops_puts("\n");

    /* 
     * Отключаем WDT в WTCON:
     * Бит 5: Watchdog Enable (0 = выключен)
     * Бит 2: Interrupt Enable (0 = выключено)
     * Бит 0: Reset Enable (0 = запрет аппаратного ресета)
     */
    wdt_write(base, WTCON_OFFSET, 0);

    /* Перезаряжаем максимальный счетчик */
    wdt_write(base, WTDAT_OFFSET, 0xFFFF);
    wdt_write(base, WTCNT_OFFSET, 0xFFFF);

    /* Сбрасываем флаг прерывания */
    wdt_write(base, WTCLRINT_OFFSET, 1);

    uint32_t con_after = wdt_read(base, WTCON_OFFSET);
    ramoops_puts("[wdt] ");
    ramoops_puts(name);
    ramoops_puts(" post-disable WTCON=");
    ramoops_put_hex((uint64_t)con_after);
    ramoops_puts("\n");
}

void wdt_disable(void) {
    ramoops_puts("[wdt] disabling hardware watchdog timers to prevent auto-reboot\n");

    disable_cluster_wdt(WDT_CL0_BASE, "Cluster0 WDT");
    disable_cluster_wdt(WDT_CL1_BASE, "Cluster1 WDT");

    /* Маскируем распространение сигнала ресета в блоке питания PMU */
    volatile uint32_t *pmu_cl0 = (volatile uint32_t *)(uintptr_t)(PMU_BASE + PMU_CL0_NONCPU_INT_EN);
    volatile uint32_t *pmu_cl1 = (volatile uint32_t *)(uintptr_t)(PMU_BASE + PMU_CL1_NONCPU_INT_EN);

    ramoops_puts("[wdt] PMU INT_EN before cl0="); ramoops_put_hex(*pmu_cl0);
    ramoops_puts(" cl1="); ramoops_put_hex(*pmu_cl1); ramoops_puts("\n");
    *pmu_cl0 &= ~(1u << 2);
    *pmu_cl1 &= ~(1u << 2);
    ramoops_puts("[wdt] PMU INT_EN after  cl0="); ramoops_put_hex(*pmu_cl0);
    ramoops_puts(" cl1="); ramoops_put_hex(*pmu_cl1); ramoops_puts("\n");

    ramoops_puts("[wdt] PMU watchdog reset lines masked\n");
    ramoops_flush();
}

void wdt_pet(void) {
    /* Регулярная подпитка счетчиков WDT для гарантии отсутствия тайм-аута */
    wdt_write(WDT_CL0_BASE, WTCNT_OFFSET, 0xFFFF);
    wdt_write(WDT_CL1_BASE, WTCNT_OFFSET, 0xFFFF);
}