/*
 * phy_usb_gs101.h — Драйвер Samsung USB 3.0 DRD PHY для Google Tensor GS101 (Pixel 6 Pro)
 *
 * Источник: vendor/stock-drivers/drivers/phy/samsung/phy-exynos-usbdrd.c,
 *           phy-exynos-usbdrd.h, gs101-raviole-usb.dtsi
 */

#ifndef NEXUS_PHY_USB_GS101_H
#define NEXUS_PHY_USB_GS101_H

#include <stdint.h>

/* Базовые адреса */
#define GS101_USBDRD_PHY_BASE    0x11100000ULL
#define GS101_PMU_USBDRD_ISOL    0x17463EB0ULL  /* PMU offset 0x3EB0 */
#define GS101_PMU_USBDP_ISOL     0x17463EB4ULL  /* PMU offset 0x3EB4 */

/* Смещения регистров PHY */
#define PHY_REG_LINKSYSTEM       0x04
#define PHY_REG_PHYUTMI          0x08
#define PHY_REG_PHYPIPE          0x0C
#define PHY_REG_PHYCLKRST        0x10
#define PHY_REG_PHYREG0          0x14
#define PHY_REG_PHYREG1          0x18
#define PHY_REG_PHYPARAM0        0x1C
#define PHY_REG_PHYPARAM1        0x20
#define PHY_REG_PHYTERM          0x24
#define PHY_REG_PHYTEST          0x28
#define PHY_REG_PHYADP           0x2C
#define PHY_REG_PHYUTMICLKSEL    0x30

struct phy_usb_gs101_status {
    uint32_t pmu_isol_before;
    uint32_t pmu_isol_after;
    uint32_t link_system;
    uint32_t phy_utmi;
    uint32_t phy_clkrst;
    uint8_t  ready;
};

/* Инициализация и снятие изоляции PHY */
int  phy_usb_gs101_init(void);

/* Получение статуса для диагностики */
void phy_usb_gs101_get_status(struct phy_usb_gs101_status *out);

#endif /* NEXUS_PHY_USB_GS101_H */
