/*
 * gic_v3.h — Драйвер контроллера прерываний ARM GICv3 для Google Tensor GS101 (Pixel 6 Pro)
 *
 * Адреса из gs101.dtsi:
 *   GICD (Distributor):     0x10400000 (размер 0x10000)
 *   GICR (Redistributors):  0x10440000 (размер 0x100000, 8 ядер)
 */

#ifndef NEXUS_GIC_V3_H
#define NEXUS_GIC_V3_H

#include <stdint.h>

#define GS101_GICD_BASE         0x10400000ULL
#define GS101_GICR_BASE         0x10440000ULL

/* GICD Регистры */
#define GICD_CTLR               0x0000
#define GICD_TYPER              0x0004
#define GICD_IIDR               0x0008
#define GICD_IGROUPR(n)         (0x0080 + 4 * (n))
#define GICD_ISENABLER(n)       (0x0100 + 4 * (n))
#define GICD_ICENABLER(n)       (0x0180 + 4 * (n))
#define GICD_ISPENDR(n)         (0x0200 + 4 * (n))
#define GICD_ICPENDR(n)         (0x0280 + 4 * (n))
#define GICD_IPRIORITYR(n)      (0x0400 + 4 * (n))
#define GICD_ITARGETSR(n)       (0x0800 + 4 * (n))
#define GICD_ICFGR(n)           (0x0C00 + 4 * (n))
#define GICD_IROUTER(n)         (0x6000 + 8 * (n))

/* GICD_CTLR Биты */
#define GICD_CTLR_ARE_NS        (1U << 4)
#define GICD_CTLR_ENABLE_GRP1NS (1U << 1)
#define GICD_CTLR_ENABLE_GRP0   (1U << 0)

/* GICR Регистры (относительно базы Redistributor для конкретного ядра, шаг 0x20000) */
#define GICR_WAKER              0x0014
#define GICR_WAKER_ProcessorSleep (1U << 1)
#define GICR_WAKER_ChildrenAsleep (1U << 2)

/* SGI / PPI фрейм Redistributor (offset +0x10000) */
#define GICR_SGI_OFFSET         0x10000
#define GICR_IGROUPR0           (GICR_SGI_OFFSET + 0x0080)
#define GICR_ISENABLER0         (GICR_SGI_OFFSET + 0x0100)
#define GICR_ICENABLER0         (GICR_SGI_OFFSET + 0x0180)
#define GICR_IPRIORITYR(n)      (GICR_SGI_OFFSET + 0x0400 + 4 * (n))

struct gic_v3_status {
    uint32_t gicd_typer;
    uint32_t gicd_iidr;
    uint32_t gicr_waker;
    uint32_t max_irqs;
    uint8_t  ready;
};

typedef void (*irq_handler_t)(uint32_t irq, void *ctx);

/* Инициализация GICD, GICR CPU0 и интерфейса ICC */
int  gic_v3_init(void);

/* Управление конкретными прерываниями */
void gic_v3_enable_irq(uint32_t irq);
void gic_v3_disable_irq(uint32_t irq);
void gic_v3_set_priority(uint32_t irq, uint8_t prio);

/* Регистрация обработчика */
int  gic_v3_register_handler(uint32_t irq, irq_handler_t handler, void *ctx);

/* Диспетчеризация прерывания из векторного обработчика IRQ */
void gic_v3_dispatch(void);

/* Статус для диагностики */
void gic_v3_get_status(struct gic_v3_status *out);

#endif /* NEXUS_GIC_V3_H */
