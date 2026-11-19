/*
 * usb_gs101.h — Драйвер USB DWC3 / xHCI для Google Pixel 6 Pro (gs101)
 *
 * Аппаратная топология (из arch/arm64/boot/dts/google/gs101.dtsi):
 *   Контроллер: Synopsys DesignWare USB3 (DWC3) @ 0x11110000 (размер 0x10000)
 *   DTS: udc: usb@11110000, compatible = "samsung,exynos9-dwusb", "synopsys,dwc3"
 *   PHY: usbdrd_phy0 @ 0x11100000
 *
 * Архитектура регистров DWC3:
 *   0x0000 .. 0xBFFF : Стандартные xHCI Host Capability & Operational регистры
 *   0xC100 .. 0xC7FF : DWC3 Global Core Registers (GSNPSID, GCTL, GSTS, etc.)
 */

#ifndef NEXUS_USB_GS101_H
#define NEXUS_USB_GS101_H

#include <stdint.h>
#include "nexus_version.h"
#define USB_GS101_VERSION NEXUS_VERSION_STRING

struct usb_gs101_status {
    uint32_t dwc3_version;      /* GSNPSID: например 0x55333... ("U3...") */
    uint32_t xhci_version;       /* HCIVERSION (0x0100 = xHCI 1.0) */
    uint32_t max_ports;          /* MaxPorts из HCSPARAMS1 */
    uint32_t max_slots;          /* MaxSlots из HCSPARAMS1 */
    uint32_t usbcmd;             /* USBCMD регистр */
    uint32_t usbsts;             /* USBSTS регистр */
    uint32_t portsc1;            /* PORTSC1 регистр */
    uint8_t  connected;          /* 1 если кабель / устройство подключено (CCS) */
    uint8_t  port_enabled;       /* 1 если порт включен (PED) */
    uint8_t  port_power;         /* 1 если питание порта включено (PP) */
    uint8_t  speed;              /* Скорость: 1=FS, 2=LS, 3=HS, 4=SS */
    const char *last_error;
};

/* Инициализация USB подсистемы GS101 */
int  usb_gs101_init(void);

/* Опрос состояния портов и событий (вызывать из главного цикла) */
void usb_gs101_poll(void);

/* Получить текущий статус для UI и диагностики */
void usb_gs101_get_status(struct usb_gs101_status *out);

/* Подключен ли кабель Type-C / устройство */
int  usb_gs101_is_connected(void);

/* Строковое описание текущей скорости порта */
const char *usb_gs101_speed_str(void);

#endif
