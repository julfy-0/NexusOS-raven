/*
 * touch_raven.c — NexusOS bare-metal touchscreen driver for Pixel 6 Pro (raven / gs101)
 *
 * Hardware controller: Samsung System LSI Touchscreen IC (sec_touch, "sec,sec_ts")
 * Bus: SPI11 (USI11) @ 0x10D40000, Mode 3 (CPOL=1, CPHA=1; DTS spi-cpol+spi-cpha), 8-bit
 *
 * Pinout & Pinctrl (verified from gs101-sec-touch.dtsi & pinctrl-gs.c):
 *   SPI11 CLK/MOSI/MISO : GPP23[0..2] (func 2 = SPI11)
 *   SPI11 CS            : GPP23[3]    (func 2 = SPI11 HW CS)
 *   TS_RESET_L          : GPP26[0]    (GPIO Output, active-low)
 *   TS_SPI_SEL          : GPP26[1]    (GPIO Output, 0=AP master, 1=SLPI/AOC)
 *   TS_INT_R2_L         : GPA9[0]     (GPIO Input, active-low IRQ)
 *
 * MMIO Bases:
 *   PERIC1 pinctrl = 0x10C40000
 *     GPP23 offset 0x60 → CON=0x10C40060, DAT=0x10C40064, PUD=0x10C40068
 *     GPP26 offset 0xC0 → CON=0x10C400C0, DAT=0x10C400C4, PUD=0x10C400C8
 *   ALIVE pinctrl  = 0x174D0000
 *     GPA9  offset 0xC0 → CON=0x174D00C0, DAT=0x174D00C4, PUD=0x174D00C8
 *   SPI11 base     = 0x10D40000
 *
 * Samsung SEC Touchscreen SPI Protocol (verified from sec_ts.c / sec_ts.h):
 *   Sync byte: 0xAA (SEC_TS_SPI_SYNC_CODE)
 *   Write frame: [0xAA, Len_MSB, Len_LSB, 0x00, 0x00, Reg, Data..., Checksum, 0x00(pad to 4B)]
 *   Read request: [0xAA, 0x00, 0x01, ReadLen_MSB, ReadLen_LSB, Reg, Checksum, 0x00]
 *   Read response: 7-byte header [0xAA, Len_H, Len_L, 0x00, 0x00, Reg, Status(0x00)],
 *                  followed by ReadLen data bytes, Checksum byte, and 4-byte padding.
 *
 * Coordinate packet (8 bytes, struct sec_ts_event_coordinate):
 *   ev[0]: eid[1:0] (0=coord, 1=status, 2=gesture) | tid[5:2] (touch id 1..10) | tchsta[7:6] (1=press, 2=move, 3=release)
 *   ev[1]: X[11:4]
 *   ev[2]: Y[11:4]
 *   ev[3]: Y[3:0] in bits[3:0], X[3:0] in bits[7:4]
 *   ev[4]: major
 *   ev[5]: minor
 *   ev[6]: z[5:0] (pressure) | ttype_3_2[7:6]
 *   ev[7]: left_event[5:0] | ttype_1_0[7:6]
 *
 *   X = ((uint16_t)ev[1] << 4) | ((ev[3] >> 4) & 0x0F)
 *   Y = ((uint16_t)ev[2] << 4) | (ev[3] & 0x0F)
 *
 * Native Resolution: 1440 x 3120 (Pixel 6 Pro AMOLED)
 */

#include "a64.h"
#include "touch_raven.h"
#include "disp_draw.h"
#include "gs101_gpio.h"  /* GPIO/Pinctrl API (gs101_gpio.c) */
#include "spi_gs101.h"   /* SPI S3C64xx / USI11 API (spi_gs101.c) */

/* ── Hardware Addresses ──────────────────────────────────────────────── */
#define GS101_SPI11_BASE        0x10D40000ULL

/* Адреса банков пинов (используем константы из gs101_gpio.h) */
#define GPP23_BANK  GS101_BANK(GS101_PINCTRL_PERIC1, GPP23_OFFSET)  /* SPI11 CLK/MOSI/MISO/CS */
#define GPP26_BANK  GS101_BANK(GS101_PINCTRL_PERIC1, GPP26_OFFSET)  /* TS_RESET_L / TS_SPI_SEL */
#define GPA9_BANK   GS101_BANK(GS101_PINCTRL_ALIVE,  GPA9_OFFSET)   /* TS_INT_R2_L */

/* Совместимые алиасы — прямые адреса регистров (используются inline-диагностикой) */
#define GS101_PINCTRL_PERIC1    0x10C40000ULL
#define GS101_PINCTRL_ALIVE     0x174D0000ULL
#define GPP23_CON   (GS101_PINCTRL_PERIC1 + 0x60)
#define GPP23_DAT   (GS101_PINCTRL_PERIC1 + 0x64)
#define GPP23_PUD   (GS101_PINCTRL_PERIC1 + 0x68)
#define GPP26_CON   (GS101_PINCTRL_PERIC1 + 0xC0)
#define GPP26_DAT   (GS101_PINCTRL_PERIC1 + 0xC4)
#define GPP26_PUD   (GS101_PINCTRL_PERIC1 + 0xC8)
#define GPA9_CON    (GS101_PINCTRL_ALIVE + 0xC0)
#define GPA9_DAT    (GS101_PINCTRL_ALIVE + 0xC4)
#define GPA9_PUD    (GS101_PINCTRL_ALIVE + 0xC8)

/* ── SPI регистры: алиасы к именам spi_gs101.h для совместимости ──────── */
#define REG_SPI_CH_CFG      S3C_SPI_CH_CFG
#define REG_SPI_CLK_CFG     S3C_SPI_CLK_CFG
#define REG_SPI_MODE_CFG    S3C_SPI_MODE_CFG
#define REG_SPI_SLAVE_SEL   S3C_SPI_SLAVE_SEL
#define REG_SPI_INT_EN      S3C_SPI_INT_EN
#define REG_SPI_STATUS      S3C_SPI_STATUS
#define REG_SPI_TX_DATA     S3C_SPI_TX_DATA
#define REG_SPI_RX_DATA     S3C_SPI_RX_DATA
#define REG_SPI_PACKET_CNT  S3C_SPI_PACKET_CNT
#define REG_SPI_PENDING_CLR S3C_SPI_PENDING_CLR
#define REG_SPI_SWAP_CFG    S3C_SPI_SWAP_CFG
#define REG_SPI_FB_CLK_SEL  S3C_SPI_FB_CLK_SEL

/* Биты CH_CFG: алиасы к spi_gs101.h (там те же значения, нет конфликта) */
#define SPI_CH_SLAVE        SPI_CH_SLAVE_MODE
#define SPI_SLAVE_INACTIVE  (1U << 0)

/* ── Samsung SEC Touchscreen Definitions (sec_ts.h) ─────────────────── */
#define SEC_TS_SPI_SYNC_CODE            0xAA
#define SEC_TS_SPI_HEADER_SIZE          5
#define SEC_TS_SPI_READ_HEADER_SIZE     7
#define SEC_TS_SPI_CHECKSUM_SIZE        1

#define SEC_TS_SPI_CMD_OK               0x00

/* Commands & Registers */
#define SEC_TS_CMD_SENSE_ON             0x10
#define SEC_TS_CMD_SENSE_OFF            0x11
#define SEC_TS_CMD_SW_RESET             0x12
#define SEC_TS_CMD_SET_TOUCHFUNCTION    0x30
/* TOUCH | PALM | WET — SEC_TS_DEFAULT_ENABLE_BIT_SETFUNC из sec_ts.h */
#define SEC_TS_DEFAULT_TOUCHFUNC        0x0061
#define SEC_TS_READ_FIRMWARE_INTEGRITY  0x21
#define SEC_TS_READ_DEVICE_ID           0x22
#define SEC_TS_READ_PANEL_INFO          0x23
#define SEC_TS_READ_ID                  0x52
#define SEC_TS_READ_BOOT_STATUS         0x55
#define SEC_TS_READ_ONE_EVENT           0x60
#define SEC_TS_READ_ALL_EVENT           0x61
#define SEC_TS_CMD_CLEAR_EVENT_STACK    0x62
#define SEC_TS_READ_TS_STATUS           0xAF

/* Event IDs (bits [1:0] of byte 0) */
#define SEC_TS_COORDINATE_EVENT         0
#define SEC_TS_STATUS_EVENT             1
#define SEC_TS_GESTURE_EVENT            2
#define SEC_TS_EMPTY_EVENT              3

/* Coordinate Actions (tchsta, bits [7:6] of byte 0) */
#define SEC_TS_COORDINATE_ACTION_NONE   0
#define SEC_TS_COORDINATE_ACTION_PRESS  1
#define SEC_TS_COORDINATE_ACTION_MOVE   2
#define SEC_TS_COORDINATE_ACTION_RELEASE 3

/* Status Event Types */
#define TYPE_STATUS_EVENT_CMD_DRIVEN    0
#define TYPE_STATUS_EVENT_ERR           1
#define TYPE_STATUS_EVENT_INFO          2

/* Boot Status */
#define SEC_TS_ACK_BOOT_COMPLETE        0x00
#define SEC_TS_STATUS_BOOT_MODE         0x10
#define SEC_TS_STATUS_APP_MODE          0x20

/* Panel Native Dimensions */
#define TOUCH_PANEL_MAX_X               1440
#define TOUCH_PANEL_MAX_Y               3120

/* ── Driver State ────────────────────────────────────────────────────── */
static struct touch_state      g_last_touch;
static struct touch_debug_info  g_dbg;
static int                     g_touch_initialized = 0;
static uint16_t                g_panel_max_x = TOUCH_PANEL_MAX_X;
static uint16_t                g_panel_max_y = TOUCH_PANEL_MAX_Y;

/* ── Delays ──────────────────────────────────────────────────────────── */
static inline void nop_delay(int n) {
    for (volatile int i = 0; i < n; i++) __asm__ volatile("nop");
}

/* ── SPI Chip Select ─────────────────────────────────────────────────── */
/* CS в Linux — это GPIO (DTS: cs-gpios = <&gpp23 3 ...>), активный низкий.
 * Ведём его вручную через GPP23[3]; SLAVE_SEL держим в "активном" ручном режиме. */
static inline void spi_cs_assert(void) {
    wr32(GPP23_DAT, rd32(GPP23_DAT) & ~(1U << 3));
}
static inline void spi_cs_deassert(void) {
    wr32(GPP23_DAT, rd32(GPP23_DAT) | (1U << 3));
}

/* ── IRQ Line Check: GPA9[0] ─────────────────────────────────────────── */
/* TS_INT_R2_L is active low: when pulled low by the IC, events are waiting */
static inline int touch_irq_pending(void) {
    return (rd32(GPA9_DAT) & 0x01U) == 0;
}

/* ── Диагностика (только лог; тесты явно помечены) ───────────────────── */
#define SPI_PACKET_CNT_EN (1U << 16)   /* S3C64XX_SPI_PACKET_CNT_EN, spi-s3c64xx.c:98 */
#define SPI_RX_LVL(st)    (((st) >> 15) & 0x1FFU)  /* rx_lvl_offset = 15 у всех exynos-вариантов (spi-s3c64xx.c:2158-2207) */
#define SPI_XFER_CHUNK    32

static uint32_t g_rx_n00, g_rx_nff, g_rx_naa, g_rx_nother, g_spi_to;
static int      g_raw_budget = 12;   /* сколько сырых кадров вывести в лог */

static void dbg_kv(const char *k, uint64_t v) {
    ramoops_puts(" "); ramoops_puts(k); ramoops_puts("="); ramoops_put_hex(v);
}
static void dbg_hex8(uint8_t v) {
    char b[3];
    b[0] = "0123456789abcdef"[v >> 4]; b[1] = "0123456789abcdef"[v & 15]; b[2] = 0;
    ramoops_puts(b);
}
static void dbg_dump_regs(const char *tag) {
    uint64_t b = GS101_SPI11_BASE;
    ramoops_puts("[touch] REGS@"); ramoops_puts(tag); ramoops_puts("\n  GPP23");
    dbg_kv("CON", rd32(GPP23_CON)); dbg_kv("DAT", rd32(GPP23_DAT)); dbg_kv("PUD", rd32(GPP23_PUD));
    ramoops_puts("\n  GPP26");
    dbg_kv("CON", rd32(GPP26_CON)); dbg_kv("DAT", rd32(GPP26_DAT)); dbg_kv("PUD", rd32(GPP26_PUD));
    ramoops_puts("\n  GPA9 ");
    dbg_kv("CON", rd32(GPA9_CON)); dbg_kv("DAT", rd32(GPA9_DAT)); dbg_kv("PUD", rd32(GPA9_PUD));
    ramoops_puts("\n  SPI  ");
    dbg_kv("CH", rd32(b + REG_SPI_CH_CFG)); dbg_kv("CLK", rd32(b + REG_SPI_CLK_CFG));
    dbg_kv("MODE", rd32(b + REG_SPI_MODE_CFG)); dbg_kv("SSEL", rd32(b + REG_SPI_SLAVE_SEL));
    dbg_kv("ST", rd32(b + REG_SPI_STATUS)); dbg_kv("PKT", rd32(b + REG_SPI_PACKET_CNT));
    dbg_kv("FB", rd32(b + REG_SPI_FB_CLK_SEL)); dbg_kv("USI_CON", rd32(b + 0xC4));
    dbg_kv("SW_CONF", rd32(0x10C21000ULL + 0x0C));
    ramoops_puts("\n");
}
/* Пин INT (GPA9[0], active-low) каждые 5 мс: строка '0'/'1' + число нулей. */
static void dbg_int_trace(const char *tag, int ms) {
    char buf[64]; int n = ms / 5, lows = 0;
    if (n > 60) n = 60;
    for (int i = 0; i < n; i++) {
        int v = (int)(rd32(GPA9_DAT) & 1);
        buf[i] = v ? '1' : '0'; if (!v) lows++;
        delay_ms(5);
    }
    buf[n] = 0;
    ramoops_puts("[touch] INT trace "); ramoops_puts(tag); ramoops_puts(": "); ramoops_puts(buf);
    dbg_kv("lows", (uint64_t)lows); ramoops_puts("\n");
}
/* Тест MISO (GPP23[2]) как GPIO-входа с подтяжкой вниз/вверх, CS high и CS low.
 * Если IC жив и выбран (CS low), он ведёт линию и значение НЕ следует за подтяжкой.
 * Если при CS low значение следует за подтяжкой, IC не отвечает (питание/сброс/селектор).
 * PUD: 4 бита на пин (pinctrl-gs.c, bank_type_6); коды 1=pull-down, 3=pull-up — ПРЕДПОЛОЖЕНИЕ (pinctrl-exynos.h). */
static void dbg_miso_probe(void) {
    uint32_t con0 = rd32(GPP23_CON), pud0 = rd32(GPP23_PUD), r[4];
    wr32(GPP23_CON, con0 & ~(0xFU << 8));            /* GPP23[2] -> input */
    for (int cs = 0; cs < 2; cs++) {
        if (cs) spi_cs_assert(); else spi_cs_deassert();
        wr32(GPP23_PUD, (pud0 & ~(0xFU << 8)) | (1U << 8));
        delay_ms(1);
        r[cs * 2]     = (rd32(GPP23_DAT) >> 2) & 1;  /* pull-down */
        wr32(GPP23_PUD, (pud0 & ~(0xFU << 8)) | (3U << 8));
        delay_ms(1);
        r[cs * 2 + 1] = (rd32(GPP23_DAT) >> 2) & 1;  /* pull-up */
    }
    spi_cs_deassert();
    wr32(GPP23_PUD, pud0);
    wr32(GPP23_CON, con0);
    ramoops_puts("[touch] MISO probe (want: CS low -> value does NOT follow pull):");
    dbg_kv("csHi_pd", r[0]); dbg_kv("csHi_pu", r[1]);
    dbg_kv("csLo_pd", r[2]); dbg_kv("csLo_pu", r[3]);
    ramoops_puts("\n");
}
static const char *g_reason = "not probed";
const char *touch_raven_reason(void) { return g_reason; }

static void dbg_verdict(int dev_ok) {
    uint32_t tot = g_rx_n00 + g_rx_nff + g_rx_naa + g_rx_nother;
    const char *ru, *en;
    ramoops_puts("[touch] SUMMARY rx");
    dbg_kv("n00", g_rx_n00); dbg_kv("nFF", g_rx_nff); dbg_kv("nAA", g_rx_naa);
    dbg_kv("other", g_rx_nother); dbg_kv("spi_timeouts", g_spi_to);
    if (dev_ok)                     { ru = "контроллер отвечает: DEVICE_ID прочитан\n"; en = "controller answers (device id read)"; }
    else if (g_spi_to && !tot)      { ru = "SPI block не работает (таймауты FIFO): клоки USI11 / SW_CONF / USI_CON\n"; en = "SPI11 FIFO timeout (clock/USI)"; }
    else if (!tot)                  { ru = "нет принятых данных\n"; en = "no rx data"; }
    else if (g_rx_naa)              { ru = "0xAA виден в ответе: IC жив, смотреть протокол/заголовок\n"; en = "0xAA seen, frame/protocol mismatch"; }
    else if (g_rx_nother)           { ru = "MISO меняется, но 0xAA нет: режим SPI / тайминг / фрейминг\n"; en = "MISO toggles, no 0xAA (mode/timing)"; }
    else if (g_rx_nff && !g_rx_n00) { ru = "MISO всегда 1: IC без питания/в reset/не выбран или линия не подключена\n"; en = "MISO stuck high (no power/reset?)"; }
    else if (g_rx_n00 && !g_rx_nff) { ru = "MISO всегда 0: линия прижата, IC не отвечает\n"; en = "MISO stuck low, IC silent"; }
    else                            { ru = "смесь 00/FF без других байтов: шина шумит или плавает\n"; en = "bus floating (00/FF only)"; }
    g_reason = en;
    ramoops_puts("\n[touch] VERDICT: "); ramoops_puts(ru);
}

/* ── GPIO Initialization — через gs101_gpio API ──────────────────────── */
static void touch_gpio_init(void) {
    /*
     * 1. GPP23[0..2] → SPI11 func=2 (CLK, MOSI, MISO), без подтяжек
     *    GPP23[3]    → GPIO Output (CS ведём вручную, активный низкий)
     */
    gs101_gpio_cfg_pin(GPP23_BANK, 0, 2);  /* CLK  → SPI11 */
    gs101_gpio_cfg_pin(GPP23_BANK, 1, 2);  /* MOSI → SPI11 */
    gs101_gpio_cfg_pin(GPP23_BANK, 2, 2);  /* MISO → SPI11 */
    gs101_gpio_cfg_pin(GPP23_BANK, 3, 1);  /* CS   → GPIO Output */
    gs101_gpio_set_pull(GPP23_BANK, 0, GPIO_PULL_NONE);
    gs101_gpio_set_pull(GPP23_BANK, 1, GPIO_PULL_NONE);
    gs101_gpio_set_pull(GPP23_BANK, 2, GPIO_PULL_NONE);
    gs101_gpio_set_pull(GPP23_BANK, 3, GPIO_PULL_NONE);
    /* CS неактивен (high) до начала транзакции */
    gs101_gpio_set_val(GPP23_BANK, 3, 1);

    /*
     * 2. GPP26[0] = TS_RESET_L → Output (func=1)
     *    GPP26[1] = TS_SPI_SEL → Output (func=1, 0 = AP Master)
     */
    gs101_gpio_cfg_pin(GPP26_BANK, 0, 1);   /* TS_RESET_L → Output */
    gs101_gpio_cfg_pin(GPP26_BANK, 1, 1);   /* TS_SPI_SEL → Output */
    gs101_gpio_set_val(GPP26_BANK, 1, 0);   /* AP Master mode */

    /*
     * 3. Hardware Reset Sequence (Linux sec_ts.c):
     *    HIGH(10ms) → LOW(10ms) → HIGH → wait 70ms
     */
    gs101_gpio_set_val(GPP26_BANK, 0, 1);
    delay_ms(10);
    ramoops_puts("[touch] INT before reset-low=");
    ramoops_put_hex((uint64_t)gs101_gpio_get_val(GPA9_BANK, 0));
    ramoops_puts("\n");

    gs101_gpio_set_val(GPP26_BANK, 0, 0);
    delay_ms(10);
    ramoops_puts("[touch] INT during reset-low=");
    ramoops_put_hex((uint64_t)gs101_gpio_get_val(GPA9_BANK, 0));
    ramoops_puts("\n");

    gs101_gpio_set_val(GPP26_BANK, 0, 1);
    delay_ms(70);

    /*
     * 4. GPA9[0] = TS_INT_R2_L → Input (func=0), pull-up
     */
    gs101_gpio_cfg_pin(GPA9_BANK, 0, 0);        /* Input */
    gs101_gpio_set_pull(GPA9_BANK, 0, GPIO_PULL_UP);
}

/* ── USI11 SPI: адреса теперь живут в spi_gs101.h — удалены дубли ────── */
/* Оставляем только алиасы для диагностических строк логов */
#define SYSREG_PERIC1_USI11_SW_CONF  (GS101_SYSREG_PERIC1 + GS101_USI11_SW_CONF_OFFSET)
/* USI_SW_CONF_SPI и CMU_PERIC1_BASE — из spi_gs101.h */
#define CMU_PERIC1_BASE              GS101_CMU_PERIC1

/* добавить рядом со статиками */
static uint32_t g_spi_mode = SPI_CPOL_HIGH | SPI_CPHA_B;

/* Конфигурация SPI-контроллера (делегируется в spi_gs101_init/transfer) */
static struct spi_gs101_cfg g_spi_cfg = {
    .base    = GS101_SPI11_BASE,
    .mode    = SPI_CPOL_HIGH | SPI_CPHA_B,
    .clk_div = 12,   /* 26 MHz / (2 * 13) = 1 MHz — оптимально для SEC_TS */
};

static void spi_init(void);
static int  sec_ts_spi_read(uint8_t reg, uint8_t *data, int len);

static void touch_set_spi_sel(int sel) {
    gs101_gpio_set_val(GPP26_BANK, 1, sel);
}

static void touch_hw_reset(void) {
    gs101_gpio_set_val(GPP26_BANK, 0, 0); delay_ms(10);
    gs101_gpio_set_val(GPP26_BANK, 0, 1); delay_ms(120); /* sec_ts: boot до ~100 мс */
}

static int touch_try_devid(void) {
    uint8_t id[5] = {0};
    for (int i = 0; i < 3; i++) {
        if (sec_ts_spi_read(SEC_TS_READ_DEVICE_ID, id, 5) == 0) return 0;
        delay_ms(20);
    }
    return -1;
}

static int touch_autoprobe(void) {
    static const uint32_t modes[2] = { SPI_CPOL_HIGH | SPI_CPHA_B, 0 };
    for (int sel = 0; sel < 2; sel++) {
        for (int m = 0; m < 2; m++) {
            g_spi_mode = modes[m];
            touch_set_spi_sel(sel);
            touch_hw_reset();
            spi_init();
            ramoops_puts("[touch] autoprobe sel="); ramoops_put_hex(sel);
            ramoops_puts(" mode="); ramoops_put_hex(g_spi_mode); ramoops_puts("\n");
            if (touch_try_devid() == 0) {
                ramoops_puts("[touch] autoprobe: OK\n");
                return 0;
            }
        }
    }
    g_spi_mode = SPI_CPOL_HIGH | SPI_CPHA_B;
    touch_set_spi_sel(0);
    return -1;
}

/* ── SPI Initialization — делегация в spi_gs101 ─────────────────────── */
static void spi_init(void) {
    /* Синхронизируем режим из g_spi_mode (autoprobe может его менять) */
    g_spi_cfg.mode = g_spi_mode;
    spi_gs101_init(&g_spi_cfg);
    spi_cs_deassert();
}

/* ── SPI Reset FIFO — делегация в spi_gs101 ─────────────────────────── */
static void spi_flush_reset(void) {
    spi_gs101_flush(&g_spi_cfg);
}

/* ── SPI Transfer (порядок как в s3c64xx_enable_datapath, PIO) ───────── */
/* spi-s3c64xx.c:565-575: PACKET_CNT_EN снять, записать счётчик, снова поднять EN;
 * затем TX FIFO; затем CH_CFG с TXCH_ON|RXCH_ON. RX всегда включён. */
static int spi_xfer_chunk(const uint8_t *tx, uint8_t *rx, int n) {
    uint64_t base = GS101_SPI11_BASE;
    uint32_t cfg = g_spi_mode;
    uint32_t to = 200000;

    spi_flush_reset();
    wr32(base + REG_SPI_PACKET_CNT, 0);
    wr32(base + REG_SPI_PACKET_CNT, (uint32_t)n);
    wr32(base + REG_SPI_PACKET_CNT, (uint32_t)n | SPI_PACKET_CNT_EN);
    for (int i = 0; i < n; i++) wr32(base + REG_SPI_TX_DATA, tx ? tx[i] : 0U);
    wr32(base + REG_SPI_CH_CFG, cfg | SPI_CH_RXCH_ON | SPI_CH_TXCH_ON);

    while (SPI_RX_LVL(rd32(base + REG_SPI_STATUS)) < (uint32_t)n && --to);
    if (!to) {
        if (g_spi_to++ < 3) { ramoops_puts("[touch] RX LVL timeout"); dbg_kv("ST", rd32(base + REG_SPI_STATUS)); dbg_kv("n", (uint64_t)n); ramoops_puts("\n"); }
        wr32(base + REG_SPI_CH_CFG, cfg);
        wr32(base + REG_SPI_PACKET_CNT, 0);
        return -1;
    }
    for (int i = 0; i < n; i++) {
        uint8_t v = (uint8_t)(rd32(base + REG_SPI_RX_DATA) & 0xFF);
        if (rx) {
            rx[i] = v;
            if (v == 0x00) g_rx_n00++; else if (v == 0xFF) g_rx_nff++;
            else if (v == SEC_TS_SPI_SYNC_CODE) g_rx_naa++; else g_rx_nother++;
        }
    }
    wr32(base + REG_SPI_CH_CFG, cfg);
    wr32(base + REG_SPI_PACKET_CNT, 0);
    return 0;
}

static int spi_xfer(const uint8_t *tx, uint8_t *rx, int n) {
    for (int off = 0; off < n; off += SPI_XFER_CHUNK) {
        int c = n - off < SPI_XFER_CHUNK ? n - off : SPI_XFER_CHUNK;
        if (spi_xfer_chunk(tx ? tx + off : 0, rx ? rx + off : 0, c) < 0) return -1;
    }
    return 0;
}

/* ── Samsung SEC Protocol: Write Command ─────────────────────────────── */
static int sec_ts_spi_write(uint8_t reg, const uint8_t *data, int len) {
    uint8_t buf[64];
    if (len > 32) return -1;

    buf[0] = SEC_TS_SPI_SYNC_CODE;
    buf[1] = ((len + 1) >> 8) & 0xFF;
    buf[2] = (len + 1) & 0xFF;
    buf[3] = 0x00;
    buf[4] = 0x00;
    buf[5] = reg;
    for (int i = 0; i < len; i++) {
        buf[6 + i] = data[i];
    }

    int spi_len = SEC_TS_SPI_HEADER_SIZE + 1 + len;
    uint8_t checksum = 0;
    for (int i = 0; i < spi_len; i++) {
        checksum += buf[i];
    }
    buf[spi_len++] = checksum;

    /* 4-byte padding */
    for (int i = 0; i < 3; i++) buf[spi_len + i] = 0x00;
    spi_len = (spi_len + 3) & ~3;

    spi_flush_reset();
    spi_cs_assert();
    nop_delay(20);

    if (spi_xfer(buf, NULL, spi_len) < 0) {
        spi_cs_deassert();
        return -1;
    }

    nop_delay(20);
    spi_cs_deassert();
    nop_delay(50);
    return 0;
}

/* ── Samsung SEC Protocol: Read Register / Events ────────────────────── */
static int sec_ts_spi_read(uint8_t reg, uint8_t *data, int len) {
    uint8_t tx_buf[16];
    uint8_t rx_buf[128];
    if (len > 64) return -1;

    /* Phase 1: Transmit read request */
    tx_buf[0] = SEC_TS_SPI_SYNC_CODE;
    tx_buf[1] = 0x00;
    tx_buf[2] = 0x01;
    tx_buf[3] = (len >> 8) & 0xFF;
    tx_buf[4] = len & 0xFF;
    tx_buf[5] = reg;

    uint8_t write_checksum = 0;
    int spi_write_len = SEC_TS_SPI_HEADER_SIZE + 1;
    for (int i = 0; i < spi_write_len; i++) {
        write_checksum += tx_buf[i];
    }
    tx_buf[spi_write_len++] = write_checksum;
    for (int i = 0; i < 3; i++) tx_buf[spi_write_len + i] = 0x00;
    spi_write_len = (spi_write_len + 3) & ~3;

    spi_flush_reset();
    spi_cs_assert();
    nop_delay(20);

    if (spi_xfer(tx_buf, NULL, spi_write_len) < 0) {
        spi_cs_deassert();
        return -1;
    }

    nop_delay(20);
    spi_cs_deassert();

    /* Phase 2: Processing delay (from sec_ts_spi_delay) */
    if (reg == SEC_TS_READ_FIRMWARE_INTEGRITY) {
        delay_ms(20);
    } else if (reg == SEC_TS_READ_ALL_EVENT) {
        nop_delay(2000); /* ~550 us */
    } else {
        nop_delay(400);  /* ~100 us */
    }

    /* Phase 3: Read response */
    int spi_read_len = len + SEC_TS_SPI_READ_HEADER_SIZE + SEC_TS_SPI_CHECKSUM_SIZE;
    spi_read_len = (spi_read_len + 3) & ~3;

    spi_flush_reset();
    spi_cs_assert();
    nop_delay(20);

    if (spi_xfer(NULL, rx_buf, spi_read_len) < 0) {
        spi_cs_deassert();
        return -2;
    }

    nop_delay(20);
    spi_cs_deassert();
    nop_delay(50);

    if (g_raw_budget > 0) {
        g_raw_budget--;
        ramoops_puts("[touch] RAW reg="); dbg_hex8(reg); ramoops_puts(" rx:");
        for (int i = 0; i < spi_read_len && i < 16; i++) { ramoops_puts(" "); dbg_hex8(rx_buf[i]); }
        ramoops_puts("\n");
    }

    /* Сохраняем сырой заголовок ДО проверки — нужен для диагностики */
    for (int i = 0; i < 7; i++) g_dbg.last_rx_header[i] = rx_buf[i];

    /* Verify response framing & checksum */
    uint8_t read_checksum = 0;
    for (int i = 0; i < SEC_TS_SPI_READ_HEADER_SIZE + len; i++) {
        read_checksum += rx_buf[i];
    }

    if (rx_buf[0] != SEC_TS_SPI_SYNC_CODE || rx_buf[5] != reg ||
        read_checksum != rx_buf[SEC_TS_SPI_READ_HEADER_SIZE + len]) {
        return -3;   /* кадр пришёл, но не по протоколу (см. last_rx_header) */
    }

    /* Save header to debug struct */
    for (int i = 0; i < 7; i++) {
        g_dbg.last_rx_header[i] = rx_buf[i];
    }

    /* Copy payload */
    for (int i = 0; i < len; i++) {
        data[i] = rx_buf[SEC_TS_SPI_READ_HEADER_SIZE + i];
    }
    return 0;
}

/* ── Public Driver API ───────────────────────────────────────────────── */

int touch_raven_init(void) {
    int dev_ok = 0;
    ramoops_puts("[touch] init Samsung SEC TS on GS101 SPI11 (0x10D40000)\n");

    /* Reset state structures */
    g_last_touch.active        = 0;
    g_last_touch.num_points    = 0;
    g_last_touch.event_counter = 0;
    for (int i = 0; i < 7; i++) g_dbg.last_rx_header[i]  = 0;
    for (int i = 0; i < 8; i++) g_dbg.last_rx_payload[i] = 0;
    g_dbg.poll_count   = 0;
    g_dbg.packet_count = 0;
    g_dbg.spi_status   = 0;
    g_dbg.hw_active    = 0;
    g_dbg.last_x       = 0;
    g_dbg.last_y       = 0;

    /* 1. Hardware Pins + Reset Pulse */
    dbg_dump_regs("before gpio_init");
    touch_gpio_init();
    dbg_int_trace("after reset", 300);
    dbg_miso_probe();

    /* 2. SPI Controller Initialization */
    spi_init();
    dbg_dump_regs("after spi_init");

    /* Диагностика: живы ли пины и блок SPI */
    ramoops_puts("[touch] diag GPP23_CON=");  ramoops_put_hex(rd32(GPP23_CON));
    ramoops_puts(" GPP26_DAT=");              ramoops_put_hex(rd32(GPP26_DAT));
    ramoops_puts(" GPA9_DAT(int)=");          ramoops_put_hex(rd32(GPA9_DAT));
    ramoops_puts("\n[touch] diag SPI CH_CFG=");  ramoops_put_hex(rd32(GS101_SPI11_BASE + REG_SPI_CH_CFG));
    ramoops_puts(" CLK_CFG=");                ramoops_put_hex(rd32(GS101_SPI11_BASE + REG_SPI_CLK_CFG));
    ramoops_puts(" STATUS=");                 ramoops_put_hex(rd32(GS101_SPI11_BASE + REG_SPI_STATUS));
    ramoops_puts(" SLAVE_SEL=");              ramoops_put_hex(rd32(GS101_SPI11_BASE + REG_SPI_SLAVE_SEL));
    ramoops_puts("\n");
    ramoops_flush();

    /* 3. Wait for Boot Complete */
    ramoops_puts("[touch] waiting BOOT_COMPLETE...\n");
    int boot_ready = 0;
    for (int retry = 0; retry < 50; retry++) {
        uint8_t ev[8];
        int rr = sec_ts_spi_read(SEC_TS_READ_ONE_EVENT, ev, 8);
        if (retry < 3 || rr == 0) {
            ramoops_puts("[touch] boot-wait read rc=");  ramoops_put_hex((uint64_t)(int64_t)rr);
            ramoops_puts(" hdr=");
            for (int i = 0; i < 7; i++) { ramoops_put_hex(g_dbg.last_rx_header[i]); ramoops_puts(" "); }
            ramoops_puts(" int_pin="); ramoops_put_hex(rd32(GPA9_DAT) & 1);
            ramoops_puts("\n");
        }
        if (rr == 0) {
            uint8_t eid = ev[0] & 0x03;
            uint8_t stype = (ev[0] >> 2) & 0x0F;
            if (eid == SEC_TS_STATUS_EVENT && stype == TYPE_STATUS_EVENT_INFO &&
                ev[1] == SEC_TS_ACK_BOOT_COMPLETE) {
                ramoops_puts("[touch] BOOT_COMPLETE ack received\n");
                boot_ready = 1;
                break;
            }
        }
        delay_ms(20);
    }

    if (!boot_ready) {
        /* Check boot status register fallback */
        uint8_t boot_stat = 0;
        if (sec_ts_spi_read(SEC_TS_READ_BOOT_STATUS, &boot_stat, 1) == 0) {
            ramoops_puts("[touch] boot_status=");
            ramoops_put_hex(boot_stat);
            ramoops_puts("\n");
        }
    }

    /* 4. Query Device Information */
    uint8_t dev_id[5] = {0};
    if (sec_ts_spi_read(SEC_TS_READ_DEVICE_ID, dev_id, 5) == 0) {
        dev_ok = 1;
        ramoops_puts("[touch] Samsung SEC Device ID: ");
        for (int i = 0; i < 5; i++) {
            ramoops_put_hex(dev_id[i]);
            ramoops_puts(" ");
        }
        ramoops_puts("\n");
    }

    else {
        ramoops_puts("[touch] DEVICE_ID read FAILED -> controller not answering over SPI\n");
    }

    if (!dev_ok && touch_autoprobe() == 0) dev_ok = 1;

    uint8_t pinfo[11] = {0};
    if (sec_ts_spi_read(SEC_TS_READ_PANEL_INFO, pinfo, 11) == 0) {
        uint16_t rx = (pinfo[0] << 8) | pinfo[1];
        uint16_t ry = (pinfo[2] << 8) | pinfo[3];
        if (rx > 0 && ry > 0) {
            g_panel_max_x = rx;
            g_panel_max_y = ry;
        }
        ramoops_puts("[touch] panel res: ");
        ramoops_put_hex(g_panel_max_x);
        ramoops_puts(" x ");
        ramoops_put_hex(g_panel_max_y);
        ramoops_puts("\n");
    }

    /* 5a. Как в Linux sec_ts_start_device(): сначала SET_TOUCHFUNCTION (2 байта, LE) */
    {
        uint8_t tf[2] = { SEC_TS_DEFAULT_TOUCHFUNC & 0xFF, (SEC_TS_DEFAULT_TOUCHFUNC >> 8) & 0xFF };
        int wr = sec_ts_spi_write(SEC_TS_CMD_SET_TOUCHFUNCTION, tf, 2);
        ramoops_puts("[touch] SET_TOUCHFUNCTION rc="); ramoops_put_hex((uint64_t)(int64_t)wr); ramoops_puts("\n");
        delay_ms(5);
    }

    /* 5b. Activate Touch Sensing (SEC_TS_CMD_SENSE_ON) */
    ramoops_puts("[touch] activating SENSE_ON\n");
    sec_ts_spi_write(SEC_TS_CMD_SENSE_ON, NULL, 0);
    delay_ms(20);

    /* 6. Clear Any Lingering Event Queue */
    sec_ts_spi_write(SEC_TS_CMD_CLEAR_EVENT_STACK, NULL, 0);
    delay_ms(5);

    dbg_int_trace("after SENSE_ON", 200);
    dbg_dump_regs("end of init");
    dbg_verdict(dev_ok);
    g_touch_initialized = 1;
    ramoops_puts("[touch] Samsung SEC Touchscreen driver ready\n");
    ramoops_flush();
    return dev_ok ? 0 : -1;
}

static struct touch_point g_slots[MAX_TOUCH_POINTS];
static struct touch_point g_released;

int touch_raven_poll(struct touch_state *st) {
    if (!g_touch_initialized || !st) return 0;
    g_dbg.poll_count++;
    g_dbg.spi_status = rd32(GS101_SPI11_BASE + REG_SPI_STATUS);

    int irq = touch_irq_pending();
    if (!irq && (g_dbg.poll_count % 25) != 0) return 0;

    int changed = 0;
    for (int n = 0; n < 16; n++) {
        uint8_t ev[8];
        int rc = sec_ts_spi_read(SEC_TS_READ_ONE_EVENT, ev, 8);
        if (rc < 0) {
            static uint32_t errs;
            if ((errs++ % 250) == 0) {
                ramoops_puts("[touch] poll err rc="); ramoops_put_hex((uint64_t)(int64_t)rc);
                ramoops_puts(" irq="); ramoops_put_hex((uint64_t)irq); ramoops_puts("\n");
            }
            break;
        }
        for (int i = 0; i < 8; i++) g_dbg.last_rx_payload[i] = ev[i];
        if (ev[0] == 0) break;

        uint8_t eid = ev[0] & 3;
        if (eid == SEC_TS_EMPTY_EVENT) break;

        if (eid == SEC_TS_STATUS_EVENT) {
            uint8_t stype = (ev[0] >> 2) & 0xF;
            if (stype == TYPE_STATUS_EVENT_INFO && ev[1] == SEC_TS_ACK_BOOT_COMPLETE)
                sec_ts_spi_write(SEC_TS_CMD_SENSE_ON, NULL, 0);
            continue;
        }
        if (eid != SEC_TS_COORDINATE_EVENT) continue;

        uint8_t tid = (ev[0] >> 2) & 0xF;
        uint8_t sta = (ev[0] >> 6) & 3;
        if (tid == 0 || tid > MAX_TOUCH_POINTS) continue;

        struct touch_point *p = &g_slots[tid - 1];
        uint16_t x = ((uint16_t)ev[1] << 4) | ((ev[3] >> 4) & 0xF);
        uint16_t y = ((uint16_t)ev[2] << 4) | (ev[3] & 0xF);
        if (x >= g_panel_max_x) x = g_panel_max_x - 1;
        if (y >= g_panel_max_y) y = g_panel_max_y - 1;

        p->x = x; p->y = y; p->id = tid - 1; p->pressure = ev[6] & 0x3F;
        p->pressed = (sta == SEC_TS_COORDINATE_ACTION_PRESS ||
                      sta == SEC_TS_COORDINATE_ACTION_MOVE);
        if (!p->pressed) g_released = *p;

        g_dbg.packet_count++; g_dbg.hw_active = 1; g_dbg.last_x = x; g_dbg.last_y = y;
        changed = 1;
        if ((ev[7] & 0x3F) == 0 && !irq) break;
    }
    if (!changed) return 0;

    int k = 0;
    st->active = 0;
    for (int i = 0; i < MAX_TOUCH_POINTS; i++)
        if (g_slots[i].pressed) st->points[k++] = g_slots[i];
    if (k == 0) { st->points[0] = g_released; st->points[0].pressed = 0; k = 1; }
    else st->active = 1;
    st->num_points = (uint8_t)k;
    st->event_counter++;
    g_last_touch = *st;
    return 1;
}

void touch_raven_get_debug(struct touch_debug_info *dbg) {
    if (dbg) *dbg = g_dbg;
}

void touch_raven_render_feedback(struct disp_state *s, const struct touch_state *st) {
    if (!s || !s->fb || !st || !st->active) return;

    for (int i = 0; i < st->num_points && i < MAX_TOUCH_POINTS; i++) {
        if (!st->points[i].pressed) continue;

        int tx = (int)st->points[i].x;
        int ty = (int)st->points[i].y;

        /* Scale coordinates if panel resolution differs from framebuffer */
        if (s->width != g_panel_max_x && g_panel_max_x > 0)
            tx = (int)((uint64_t)tx * s->width / g_panel_max_x);
        if (s->height != g_panel_max_y && g_panel_max_y > 0)
            ty = (int)((uint64_t)ty * s->height / g_panel_max_y);

        /* Touch ripple feedback */
        disp_circle_outline(s, tx, ty, 50, 4, 0x38BDF8, 240);
        disp_circle(s, tx, ty, 30, 0x818CF8, 160);
        disp_circle(s, tx, ty, 14, 0xFFFFFF, 255);
    }
}