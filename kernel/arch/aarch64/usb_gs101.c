/*
 * usb_gs101.c — Драйвер USB DWC3 / xHCI для Google Pixel 6 Pro (gs101)
 *
 * Аппаратный контроллер: Synopsys DesignWare USB3 (DWC3) @ 0x11110000
 * Поддерживает:
 *   - Определение и опрос ядра Synopsys DWC3 (GSNPSID)
 *   - Чтение параметров xHCI Host Controller (CAPLENGTH, HCSPARAMS, USBSTS)
 *   - Мониторинг подключения Type-C кабеля / USB устройств через PORTSC1
 *   - Включение питания порта (Port Power) и контроль скорости шины
 */

#include "a64.h"
#include "usb_gs101.h"
#include "gs101_disp.h"

/* Базовый адрес DWC3 USB контроллера из gs101.dtsi */
#define GS101_USB_BASE          0x11110000ULL

/* ── Регистры DWC3 Global (смещение 0xC100) ─────────────────────────── */
#define DWC3_GSNPSID            0xC120  /* Synopsys IP ID: 'U','3',...         */
#define DWC3_GCTL               0xC110  /* Global Core Control                */
#define DWC3_GSTS               0xC118  /* Global Core Status                 */
#define DWC3_GUSB2PHYCFG0       0xC200  /* USB 2.0 PHY Config                 */
#define DWC3_GUSB3PIPECTL0      0xC2C0  /* USB 3.0 PIPE Config                */

/* ── Регистры xHCI Host Controller (смещение 0x0000) ─────────────────── */
#define XHCI_CAPLENGTH          0x00    /* Длина блока Capability             */
#define XHCI_HCIVERSION         0x02    /* Версия xHCI (0x0100)               */
#define XHCI_HCSPARAMS1         0x04    /* Структурные параметры 1             */
#define XHCI_HCSPARAMS2         0x08    /* Структурные параметры 2             */
#define XHCI_HCCPARAMS1         0x10    /* Параметры возможностей 1            */

/* Смещения операционных регистров (относительно base + CAPLENGTH) */
#define XHCI_OP_USBCMD          0x00    /* USB Command                        */
#define XHCI_OP_USBSTS          0x04    /* USB Status                         */
#define XHCI_OP_PAGESIZE        0x08    /* Page Size                          */
#define XHCI_OP_CONFIG          0x38    /* Configuration (MaxSlotsEn)         */
#define XHCI_OP_PORTSC1         0x400   /* Port 1 Status & Control            */

/* Биты PORTSC */
#define PORTSC_CCS              (1U << 0)   /* Current Connect Status (кабель) */
#define PORTSC_PED              (1U << 1)   /* Port Enabled/Disabled          */
#define PORTSC_OCA              (1U << 3)   /* Over-current Active            */
#define PORTSC_PR               (1U << 4)   /* Port Reset                     */
#define PORTSC_PLS_MASK         (0xFU << 5) /* Port Link State               */
#define PORTSC_PP               (1U << 9)   /* Port Power                     */
#define PORTSC_SPEED_MASK       (0xFU << 10)/* Port Speed                     */
#define PORTSC_SPEED_SHIFT      10

static struct usb_gs101_status g_usb_status;
static uint32_t g_op_offset = 0x20;
static int      g_initialized = 0;

int usb_gs101_init(void) {
    uint64_t base = GS101_USB_BASE;

    ramoops_puts("[usb] probing GS101 Synopsys DWC3 / xHCI @ 0x11110000\n");

    /* 1. Читаем идентификатор Synopsys IP */
    uint32_t snpsid = rd32(base + DWC3_GSNPSID);
    g_usb_status.dwc3_version = snpsid;
    ramoops_puts("[usb] DWC3 GSNPSID=");
    ramoops_put_hex(snpsid);
    ramoops_puts("\n");

    /* 2. Читаем Capability Registers xHCI */
    uint32_t cap = rd32(base + XHCI_CAPLENGTH);
    uint8_t caplen = (uint8_t)(cap & 0xFF);
    uint16_t hci_ver = (uint16_t)((cap >> 16) & 0xFFFF);

    if (caplen == 0 || caplen > 0x80) {
        caplen = 0x20; /* Типичное значение для DWC3 xHCI */
    }
    g_op_offset = caplen;
    g_usb_status.xhci_version = hci_ver;

    uint32_t hcs1 = rd32(base + XHCI_HCSPARAMS1);
    g_usb_status.max_ports = (hcs1 >> 24) & 0xFF;
    g_usb_status.max_slots = hcs1 & 0xFF;

    ramoops_puts("[usb] xHCI caplen=");
    ramoops_put_hex(caplen);
    ramoops_puts(" max_ports=");
    ramoops_put_hex(g_usb_status.max_ports);
    ramoops_puts("\n");

    /* 3. Проверяем операционные регистры */
    uint64_t op_base = base + g_op_offset;
    g_usb_status.usbcmd = rd32(op_base + XHCI_OP_USBCMD);
    g_usb_status.usbsts = rd32(op_base + XHCI_OP_USBSTS);

    /* 4. Проверяем и включаем питание порта (Port Power) на первом порту */
    uint32_t portsc = rd32(op_base + XHCI_OP_PORTSC1);
    if (!(portsc & PORTSC_PP)) {
        portsc |= PORTSC_PP;
        wr32(op_base + XHCI_OP_PORTSC1, portsc);
        delay_ms(20);
        portsc = rd32(op_base + XHCI_OP_PORTSC1);
    }
    g_usb_status.portsc1 = portsc;
    g_usb_status.connected = (portsc & PORTSC_CCS) ? 1 : 0;
    g_usb_status.port_enabled = (portsc & PORTSC_PED) ? 1 : 0;
    g_usb_status.port_power = (portsc & PORTSC_PP) ? 1 : 0;
    g_usb_status.speed = (uint8_t)((portsc & PORTSC_SPEED_MASK) >> PORTSC_SPEED_SHIFT);
    g_usb_status.last_error = "none";

    ramoops_puts("[usb] portsc1=");
    ramoops_put_hex(portsc);
    ramoops_puts(" connected=");
    ramoops_put_hex(g_usb_status.connected);
    ramoops_puts("\n");

    g_initialized = 1;
    return 1;
}

void usb_gs101_poll(void) {
    if (!g_initialized) return;

    uint64_t op_base = GS101_USB_BASE + g_op_offset;
    g_usb_status.usbsts = rd32(op_base + XHCI_OP_USBSTS);

    uint32_t portsc = rd32(op_base + XHCI_OP_PORTSC1);
    g_usb_status.portsc1 = portsc;
    g_usb_status.connected = (portsc & PORTSC_CCS) ? 1 : 0;
    g_usb_status.port_enabled = (portsc & PORTSC_PED) ? 1 : 0;
    g_usb_status.port_power = (portsc & PORTSC_PP) ? 1 : 0;
    g_usb_status.speed = (uint8_t)((portsc & PORTSC_SPEED_MASK) >> PORTSC_SPEED_SHIFT);
}

void usb_gs101_get_status(struct usb_gs101_status *out) {
    if (out) *out = g_usb_status;
}

int usb_gs101_is_connected(void) {
    return g_usb_status.connected;
}

const char *usb_gs101_speed_str(void) {
    switch (g_usb_status.speed) {
        case 1:  return "Full-Speed (12 Mbps)";
        case 2:  return "Low-Speed (1.5 Mbps)";
        case 3:  return "High-Speed USB 2.0 (480 Mbps)";
        case 4:  return "SuperSpeed USB 3.0 (5 Gbps)";
        case 5:  return "SuperSpeedPlus (10 Gbps)";
        default: return g_usb_status.connected ? "Connected" : "Disconnected";
    }
}
