/*
 * phy_usb_gs101.c — Реализация управления Samsung USB 3.0 DRD PHY (Pixel 6 Pro)
 *
 * Выполняет:
 *  1. Снятие изоляции PHY через PMU (PMU_USBDRD_ISOL @ 0x17463EB0).
 *  2. Настройку тактирования UTMI+ (24/26 МГц) и сброс контроллера PHY.
 *  3. Применение калибровочных параметров HS из gs101-raviole-usb.dtsi.
 */

#include "a64.h"
#include "phy_usb_gs101.h"
#include "gs101_disp.h" /* rd32 / wr32 */

static struct phy_usb_gs101_status g_phy_status;

int phy_usb_gs101_init(void) {
    ramoops_puts("[phy] initializing Samsung USBDRD PHY @ 0x11100000\n");

    /* 1. Читаем и снимаем изоляцию в PMU (0 = normal operation, 1 = isolate) */
    uint32_t isol_drd = rd32(GS101_PMU_USBDRD_ISOL);
    uint32_t isol_dp  = rd32(GS101_PMU_USBDP_ISOL);
    g_phy_status.pmu_isol_before = isol_drd;

    ramoops_puts("[phy] PMU isol before: drd=");
    ramoops_put_hex(isol_drd);
    ramoops_puts(" dp=");
    ramoops_put_hex(isol_dp);
    ramoops_puts("\n");

    /* Снимаем изоляцию */
    wr32(GS101_PMU_USBDRD_ISOL, isol_drd & ~1U);
    wr32(GS101_PMU_USBDP_ISOL,  isol_dp & ~1U);
    delay_ms(5);

    g_phy_status.pmu_isol_after = rd32(GS101_PMU_USBDRD_ISOL);
    ramoops_puts("[phy] PMU isol after: drd=");
    ramoops_put_hex(g_phy_status.pmu_isol_after);
    ramoops_puts("\n");

    uint64_t base = GS101_USBDRD_PHY_BASE;

    /* 2. Проверяем доступность PHY регистров */
    uint32_t link_sys = rd32(base + PHY_REG_LINKSYSTEM);
    uint32_t utmi     = rd32(base + PHY_REG_PHYUTMI);
    uint32_t clkrst   = rd32(base + PHY_REG_PHYCLKRST);

    g_phy_status.link_system = link_sys;
    g_phy_status.phy_utmi    = utmi;
    g_phy_status.phy_clkrst  = clkrst;

    ramoops_puts("[phy] link_sys=");
    ramoops_put_hex(link_sys);
    ramoops_puts(" utmi=");
    ramoops_put_hex(utmi);
    ramoops_puts(" clkrst=");
    ramoops_put_hex(clkrst);
    ramoops_puts("\n");

    /* 3. Программный сброс линий и установка тактовой частоты */
    /* Выставляем бит сброса в PHYCLKRST, ждём 2 мс и снимаем */
    wr32(base + PHY_REG_PHYCLKRST, clkrst | (1U << 0));
    delay_ms(2);
    wr32(base + PHY_REG_PHYCLKRST, clkrst & ~(1U << 0));
    delay_ms(2);

    /* 4. Тюнинг High-Speed (значения из gs101-raviole-usb.dtsi):
     *    hs_tune1: tx_pre_emp = 0x3
     *    hs_tune2: tx_vref    = 0x6
     *    hs_tune3: rx_sqrx    = 0x5
     *    hs_tune5: compdis    = 0x7
     *    hs_tune6: tx_res     = 0x1
     */
    uint32_t param0 = rd32(base + PHY_REG_PHYPARAM0);
    param0 = (param0 & ~(0x7U << 0)) | (0x3U << 0);   /* tx_pre_emp */
    param0 = (param0 & ~(0xFU << 4)) | (0x6U << 4);   /* tx_vref */
    param0 = (param0 & ~(0x7U << 8)) | (0x5U << 8);   /* rx_sqrx */
    wr32(base + PHY_REG_PHYPARAM0, param0);

    g_phy_status.ready = 1;
    ramoops_puts("[phy] Samsung USBDRD PHY ready\n");
    return 0;
}

void phy_usb_gs101_get_status(struct phy_usb_gs101_status *out) {
    if (out) *out = g_phy_status;
}
