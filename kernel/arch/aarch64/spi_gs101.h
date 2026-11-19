/*
 * spi_gs101.h — NexusOS Samsung S3C64xx SPI (USI) контроллер для GS101 (Pixel 6 Pro)
 *
 * Источник: vendor/stock-drivers/drivers/spi/spi-s3c64xx.c
 *            arch/arm64/boot/dts/google/gs101-raven.dts (SPI11 / USI11)
 *
 * Контроллер: Samsung Exynos S3C64xx IP (USI11 = Universal Serial Interface #11)
 * Базовый адрес SPI11: 0x10D40000
 * CMU PERIC1:          0x10C00000  (тактирование USI11)
 * SYSREG PERIC1:       0x10C21000  (мультиплексирование USI11 ↔ SPI/I2C/UART)
 */
#ifndef NEXUS_SPI_GS101_H
#define NEXUS_SPI_GS101_H

#include "a64.h"

/* ── Регистры Samsung S3C64xx SPI (spi-s3c64xx.c) ────────────────────── */
#define S3C_SPI_CH_CFG      0x00  /* Channel Config: mode, CPOL/CPHA, SW_RST, TX/RX enable */
#define S3C_SPI_CLK_CFG     0x04  /* Clock: prescaler, source select */
#define S3C_SPI_MODE_CFG    0x08  /* 传输参数: word width (8/16/32 bit), burst count */
#define S3C_SPI_SLAVE_SEL   0x0C  /* CS control: manual / auto */
#define S3C_SPI_INT_EN      0x10  /* Interrupt enable (не используется в polling) */
#define S3C_SPI_STATUS      0x14  /* Status: TX/RX FIFO level, busy */
#define S3C_SPI_TX_DATA     0x18  /* TX FIFO write port */
#define S3C_SPI_RX_DATA     0x1C  /* RX FIFO read port */
#define S3C_SPI_PACKET_CNT  0x20  /* Packet count + enable */
#define S3C_SPI_PENDING_CLR 0x24  /* Interrupt clear */
#define S3C_SPI_SWAP_CFG    0x28  /* Byte/bit swap */
#define S3C_SPI_FB_CLK_SEL  0x2C  /* Feedback clock delay */
#define S3C_SPI_USI_CON     0xC4  /* USI mode register (сброс перед работой) */

/* ── Биты S3C_SPI_CH_CFG ─────────────────────────────────────────────── */
#define SPI_CH_HS_EN        (1U << 6)  /* High Speed enable */
#define SPI_CH_SW_RST       (1U << 5)  /* Software reset (активен пока выставлен) */
#define SPI_CH_SLAVE_MODE   (1U << 4)  /* 0=Master, 1=Slave */
#define SPI_CPOL_HIGH       (1U << 3)  /* Clock Polarity: 0=low idle, 1=high idle */
#define SPI_CPHA_B          (1U << 2)  /* Clock Phase: 0=rising, 1=falling */
#define SPI_CH_RXCH_ON      (1U << 1)  /* RX channel enable */
#define SPI_CH_TXCH_ON      (1U << 0)  /* TX channel enable */

/* ── Биты S3C_SPI_STATUS ─────────────────────────────────────────────── */
#define SPI_ST_TX_FIFORDY   (1U << 0)   /* TX FIFO ready */
#define SPI_ST_RX_FIFORDY   (1U << 1)   /* RX FIFO ready */
/* Уровень RX FIFO в битах [23:15] (spi-s3c64xx.c строка ~2200) */
#define SPI_RX_LVL(st)      (((st) >> 15) & 0x1FFU)

/* ── Packet counter enable (PACKET_CNT регистр, бит 16) ─────────────── */
#define SPI_PKT_CNT_EN      (1U << 16)

/* ── SYSREG: мультиплексирование USI11 → SPI (spi-s3c64xx.c) ─────────── */
#define GS101_SYSREG_PERIC1         0x10C21000ULL
#define GS101_USI11_SW_CONF_OFFSET  0x0C
#define USI_SW_CONF_MASK            0x7U
#define USI_SW_CONF_SPI             0x2U   /* BIT(1) = SPI mode */

/* ── CMU PERIC1: тактирование USI11 (cmucal-sfr.c Google) ───────────── */
#define GS101_CMU_PERIC1            0x10C00000ULL
#define CMU_GATE_USI11_A            0x202CU  /* GOUT clock gate A */
#define CMU_GATE_USI11_B            0x2068U  /* GOUT clock gate B */
#define CMU_QCH_USI11               0x302CU  /* Q-channel control */
#define CMU_MUX_USI11               0x0640U  /* Mux select (только чтение для диагностики) */
#define CMU_DIV_USI11               0x180CU  /* Divider (только чтение для диагностики) */
/* Биты включения гейта: MANUAL=bit20, CG_VAL=bit21; QCH: ENABLE=bit0, CLK_REQ=bit1 */
#define CMU_GATE_EN_BITS            ((1U << 20) | (1U << 21))
#define CMU_QCH_EN_BITS             (0x3U)

/* ── Параметры передачи ──────────────────────────────────────────────── */
#define SPI_GS101_CHUNK_SIZE        32   /* максимальный burst (ограничен глубиной FIFO) */
#define SPI_GS101_RX_TIMEOUT        200000U  /* итерации ожидания RX FIFO */

/* ── Конфигурация экземпляра SPI-контроллера ─────────────────────────── */
struct spi_gs101_cfg {
    uint64_t base;      /* MMIO-адрес контроллера */
    uint32_t mode;      /* SPI_CPOL_HIGH | SPI_CPHA_B или 0 */
    uint32_t clk_div;   /* предделитель: freq = src_clk / (2 * (clk_div + 1)) */
    /* clk_div=12 → 26 MHz / 26 ≈ 1 MHz при src=26MHz (оптимально для SEC_TS на Raven) */
};

/* ── API ──────────────────────────────────────────────────────────────── */

/*
 * spi_gs101_init — инициализировать USI-блок в режиме SPI
 *   Выполняет: включение тактирования CMU, переключение USI в режим SPI,
 *   снятие USI_CON (аппаратного hold), software reset блока и программирование
 *   CPOL/CPHA, предделителя, 8-битного слова.
 */
void spi_gs101_init(const struct spi_gs101_cfg *cfg);

/*
 * spi_gs101_transfer — полнодуплексная передача N байтов (polling, PIO)
 *   tx : буфер для передачи (NULL → будет передан 0x00)
 *   rx : буфер для приёма  (NULL → принятые байты игнорируются)
 *   n  : количество байтов (разбивается на чанки SPI_GS101_CHUNK_SIZE)
 *   Возврат: 0 = OK, -1 = таймаут RX FIFO
 */
int  spi_gs101_transfer(const struct spi_gs101_cfg *cfg,
                        const uint8_t *tx, uint8_t *rx, int n);

/*
 * spi_gs101_flush — программный сброс FIFO без потери конфигурации
 */
void spi_gs101_flush(const struct spi_gs101_cfg *cfg);

#endif /* NEXUS_SPI_GS101_H */
