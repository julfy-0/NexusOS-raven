/*
 * gic_v3.c — Реализация драйвера ARM GICv3 для Pixel 6 Pro (Google Tensor GS101)
 */

#include "a64.h"
#include "gic_v3.h"
#include "gs101_disp.h" /* rd32 / wr32 */

#define MAX_HANDLERS 128

struct irq_slot {
    irq_handler_t handler;
    void *ctx;
};

static struct gic_v3_status g_status;
static struct irq_slot g_handlers[MAX_HANDLERS];

/* ── Доступ к системным регистрам CPU Interface GICv3 (EL1) ───────────── */
static inline void set_icc_pmr(uint64_t v) {
    __asm__ volatile("msr S3_0_C4_C6_0, %0" :: "r"(v) : "memory"); /* ICC_PMR_EL1 */
}

static inline void set_icc_bpr1(uint64_t v) {
    __asm__ volatile("msr S3_0_C12_C12_3, %0" :: "r"(v) : "memory"); /* ICC_BPR1_EL1 */
}

static inline void set_icc_igrpen1(uint64_t v) {
    __asm__ volatile("msr S3_0_C12_C12_7, %0" :: "r"(v) : "memory"); /* ICC_IGRPEN1_EL1 */
}

static inline uint64_t get_icc_iar1(void) {
    uint64_t v;
    __asm__ volatile("mrs %0, S3_0_C12_C12_0" : "=r"(v)); /* ICC_IAR1_EL1 */
    return v;
}

static inline void set_icc_eoir1(uint64_t v) {
    __asm__ volatile("msr S3_0_C12_C12_1, %0" :: "r"(v) : "memory"); /* ICC_EOIR1_EL1 */
}

/* ── Инициализация ───────────────────────────────────────────────────── */
int gic_v3_init(void) {
    ramoops_puts("[gic] initializing ARM GICv3 (GICD @ 0x10400000, GICR @ 0x10440000)\n");

    uint64_t gicd = GS101_GICD_BASE;
    uint64_t gicr = GS101_GICR_BASE;

    /* 1. Читаем параметры Distributor */
    uint32_t typer = rd32(gicd + GICD_TYPER);
    uint32_t iidr  = rd32(gicd + GICD_IIDR);
    uint32_t itlines = (typer & 0x1FU);
    uint32_t max_irqs = (itlines + 1) * 32;

    g_status.gicd_typer = typer;
    g_status.gicd_iidr  = iidr;
    g_status.max_irqs   = max_irqs;

    ramoops_puts("[gic] GICD_TYPER=");
    ramoops_put_hex(typer);
    ramoops_puts(" max_irqs=");
    ramoops_put_hex(max_irqs);
    ramoops_puts("\n");

    /* 2. Настраиваем GICD: включаем Group 1 Non-Secure и Affinity Routing */
    uint32_t ctlr = rd32(gicd + GICD_CTLR);
    ctlr |= GICD_CTLR_ARE_NS | GICD_CTLR_ENABLE_GRP1NS;
    wr32(gicd + GICD_CTLR, ctlr);
    __asm__ volatile("dsb sy; isb" ::: "memory");

    /* 3. Будим Redistributor ядра CPU 0 */
    uint32_t waker = rd32(gicr + GICR_WAKER);
    g_status.gicr_waker = waker;
    waker &= ~GICR_WAKER_ProcessorSleep;
    wr32(gicr + GICR_WAKER, waker);
    __asm__ volatile("dsb sy; isb" ::: "memory");

    /* Ждем, пока ядро проснется */
    for (int i = 0; i < 1000; i++) {
        if (!(rd32(gicr + GICR_WAKER) & GICR_WAKER_ChildrenAsleep)) {
            break;
        }
        delay_ms(1);
    }

    ramoops_puts("[gic] GICR CPU0 awakened, waker=");
    ramoops_put_hex(rd32(gicr + GICR_WAKER));
    ramoops_puts("\n");

    /* 4. Настраиваем CPU Interface (ICC) текущего ядра */
    set_icc_pmr(0xFF);    /* Разрешаем все приоритеты (маска 0xFF) */
    set_icc_bpr1(0x00);   /* Без группировки приоритетов */
    set_icc_igrpen1(0x01);/* Включаем обработку Group 1 прерываний */
    __asm__ volatile("isb" ::: "memory");

    /* Очищаем таблицу обработчиков */
    for (int i = 0; i < MAX_HANDLERS; i++) {
        g_handlers[i].handler = 0;
        g_handlers[i].ctx = 0;
    }

    g_status.ready = 1;
    ramoops_puts("[gic] ARM GICv3 operational\n");
    return 0;
}

void gic_v3_enable_irq(uint32_t irq) {
    if (!g_status.ready) return;

    if (irq < 32) {
        /* SGI/PPI — через GICR SGI фрейм */
        uint64_t gicr_sgi = GS101_GICR_BASE + GICR_SGI_OFFSET;
        wr32(gicr_sgi + 0x0100, 1U << irq);
    } else {
        /* SPI — через GICD */
        uint64_t gicd = GS101_GICD_BASE;
        uint32_t reg_idx = irq / 32;
        uint32_t bit_idx = irq % 32;

        /* Назначаем Group 1 Non-Secure */
        uint32_t grp = rd32(gicd + GICD_IGROUPR(reg_idx));
        grp |= (1U << bit_idx);
        wr32(gicd + GICD_IGROUPR(reg_idx), grp);

        /* Назначаем приоритет по умолчанию (0x80) */
        wr32(gicd + GICD_IPRIORITYR(irq / 4), 0x80808080U);

        /* Маршрутизация на Core 0 через IROUTER */
        uint64_t irouter_addr = gicd + GICD_IROUTER(irq);
        *(volatile uint64_t *)(uintptr_t)irouter_addr = 0ULL; /* Affinity 0.0.0.0 */

        /* Включаем прерывание в ISENABLER */
        wr32(gicd + GICD_ISENABLER(reg_idx), 1U << bit_idx);
    }
    __asm__ volatile("dsb sy; isb" ::: "memory");
}

void gic_v3_disable_irq(uint32_t irq) {
    if (!g_status.ready) return;

    if (irq < 32) {
        uint64_t gicr_sgi = GS101_GICR_BASE + GICR_SGI_OFFSET;
        wr32(gicr_sgi + 0x0180, 1U << irq);
    } else {
        uint64_t gicd = GS101_GICD_BASE;
        uint32_t reg_idx = irq / 32;
        uint32_t bit_idx = irq % 32;
        wr32(gicd + GICD_ICENABLER(reg_idx), 1U << bit_idx);
    }
    __asm__ volatile("dsb sy; isb" ::: "memory");
}

void gic_v3_set_priority(uint32_t irq, uint8_t prio) {
    if (!g_status.ready) return;

    if (irq < 32) {
        uint64_t gicr_sgi = GS101_GICR_BASE + GICR_SGI_OFFSET;
        uint32_t reg_idx = irq / 4;
        uint32_t shift = (irq % 4) * 8;
        uint32_t val = rd32(gicr_sgi + 0x0400 + reg_idx * 4);
        val = (val & ~(0xFFU << shift)) | ((uint32_t)prio << shift);
        wr32(gicr_sgi + 0x0400 + reg_idx * 4, val);
    } else {
        uint64_t gicd = GS101_GICD_BASE;
        uint32_t reg_idx = irq / 4;
        uint32_t shift = (irq % 4) * 8;
        uint32_t val = rd32(gicd + GICD_IPRIORITYR(reg_idx));
        val = (val & ~(0xFFU << shift)) | ((uint32_t)prio << shift);
        wr32(gicd + GICD_IPRIORITYR(reg_idx), val);
    }
}

int gic_v3_register_handler(uint32_t irq, irq_handler_t handler, void *ctx) {
    if (irq >= MAX_HANDLERS) return -1;
    g_handlers[irq].handler = handler;
    g_handlers[irq].ctx = ctx;
    return 0;
}

void gic_v3_dispatch(void) {
    uint32_t iar = (uint32_t)get_icc_iar1();
    uint32_t irq = iar & 0xFFFFFFU;

    /* Спец-значения: 1020..1023 — spurious / no interrupt */
    if (irq >= 1020) {
        return;
    }

    if (irq < MAX_HANDLERS && g_handlers[irq].handler) {
        g_handlers[irq].handler(irq, g_handlers[irq].ctx);
    } else {
        ramoops_puts("[gic] unhandled IRQ=");
        ramoops_put_hex((uint64_t)irq);
        ramoops_puts("\n");
    }

    /* Подтверждаем завершение обработки (End of Interrupt) */
    set_icc_eoir1((uint64_t)iar);
    __asm__ volatile("isb" ::: "memory");
}

void gic_v3_get_status(struct gic_v3_status *out) {
    if (out) *out = g_status;
}
