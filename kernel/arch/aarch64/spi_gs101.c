/*
 * spi_gs101.c — NexusOS Samsung S3C64xx SPI (USI) контроллер для GS101 (Pixel 6 Pro)
 *
 * Адаптация из:
 *   vendor/stock-drivers/drivers/spi/spi-s3c64xx.c
 *     → s3c64xx_spi_hwinit(), exynos_usi_init(), s3c64xx_enable_datapath()
 *
 * Принципы адаптации:
 *   - Убраны: Linux PM runtime, DMA engine, IRQ, regmap, clk framework.
 *   - Сохранены: точная последовательность инициализации аппаратуры, тайминги,
 *     порядок записи в PACKET_CNT перед TX FIFO, polling RX_LVL.
 */
#include "spi_gs101.h"
#include "gs101_disp.h"  /* rd32() / wr32() */

/* ── Внутренние утилиты ───────────────────────────────────────────────── */

static inline void nop_delay_spi(int n) {
    for (volatile int i = 0; i < n; i++) __asm__ volatile("nop");
}

/* ── Включение тактирования USI11 (CMU_PERIC1) ───────────────────────── */
/* Эквивалент Linux clk_prepare_enable() для USI11.
 * Значения регистров до и после пишутся в ramoops для диагностики. */
static void usi11_clk_enable(void) {
    uint64_t b = GS101_CMU_PERIC1;
    uint32_t ga = rd32(b + CMU_GATE_USI11_A);
    uint32_t gb = rd32(b + CMU_GATE_USI11_B);
    uint32_t qc = rd32(b + CMU_QCH_USI11);

    ramoops_puts("[spi11] CMU before:"
                 " ga="); ramoops_put_hex(ga);
    ramoops_puts(" gb="); ramoops_put_hex(gb);
    ramoops_puts(" qch="); ramoops_put_hex(qc);
    ramoops_puts(" mux="); ramoops_put_hex(rd32(b + CMU_MUX_USI11));
    ramoops_puts(" div="); ramoops_put_hex(rd32(b + CMU_DIV_USI11));
    ramoops_puts("\n");

    wr32(b + CMU_GATE_USI11_A, ga | CMU_GATE_EN_BITS);
    wr32(b + CMU_GATE_USI11_B, gb | CMU_GATE_EN_BITS);
    wr32(b + CMU_QCH_USI11,    qc | CMU_QCH_EN_BITS);
    delay_ms(1);

    ramoops_puts("[spi11] CMU after:"
                 " ga="); ramoops_put_hex(rd32(b + CMU_GATE_USI11_A));
    ramoops_puts(" gb="); ramoops_put_hex(rd32(b + CMU_GATE_USI11_B));
    ramoops_puts(" qch="); ramoops_put_hex(rd32(b + CMU_QCH_USI11));
    ramoops_puts("\n");
}

/* ── Переключение USI11 в режим SPI (SYSREG_PERIC1) ─────────────────── */
/* Эквивалент regmap_update_bits(usi_reg, offset, USI_SW_CONF_MASK, SPI_MODE)
 * из функции exynos_usi_init() в spi-s3c64xx.c. */
static void usi11_set_spi_mode(void) {
    uint64_t addr = GS101_SYSREG_PERIC1 + GS101_USI11_SW_CONF_OFFSET;
    uint32_t before = rd32(addr);
    wr32(addr, (before & ~USI_SW_CONF_MASK) | USI_SW_CONF_SPI);
    ramoops_puts("[spi11] USI11 SW_CONF before="); ramoops_put_hex(before);
    ramoops_puts(" after="); ramoops_put_hex(rd32(addr));
    ramoops_puts("\n");
}

/* ── Снятие аппаратного hold USI_CON ────────────────────────────────── */
/* USI_CON (+0xC4): бит USI_RESET активен-высокий — держит все пины блока
 * в стабильном состоянии. Обязательно снять до начала SPI-транзакций.
 * (найдено в spi-s3c64xx.c в комментарии к exynos_usi_init) */
static void usi11_release_reset(uint64_t base) {
    uint32_t before = rd32(base + S3C_SPI_USI_CON);
    wr32(base + S3C_SPI_USI_CON, 0);
    ramoops_puts("[spi11] USI_CON before="); ramoops_put_hex(before);
    ramoops_puts(" after="); ramoops_put_hex(rd32(base + S3C_SPI_USI_CON));
    ramoops_puts("\n");
}

/* ── Публичный API ────────────────────────────────────────────────────── */

void spi_gs101_init(const struct spi_gs101_cfg *cfg) {
    uint64_t base = cfg->base;

    ramoops_puts("[spi11] init base="); ramoops_put_hex(base);
    ramoops_puts(" mode="); ramoops_put_hex(cfg->mode);
    ramoops_puts(" clk_div="); ramoops_put_hex(cfg->clk_div);
    ramoops_puts("\n");

    /* 1. Тактирование */
    usi11_clk_enable();

    /* 2. Выбор протокола USI → SPI */
    usi11_set_spi_mode();

    /* 3. Снятие USI_CON (аппаратный hold пинов) */
    usi11_release_reset(base);

    /* 4. Выключить прерывания (работаем в polling-режиме) */
    wr32(base + S3C_SPI_INT_EN, 0);
    wr32(base + S3C_SPI_PACKET_CNT, 0);

    /* 5. Software Reset блока SPI (s3c64xx_spi_hwinit шаг 1) */
    wr32(base + S3C_SPI_CH_CFG, SPI_CH_SW_RST);
    delay_ms(2);
    wr32(base + S3C_SPI_CH_CFG, 0);
    nop_delay_spi(100);

    /* 6. Режим CPOL/CPHA (Mode 0 или Mode 3), Master, TX+RX пока выключены
     *    (TX/RX включаются в transfer непосредственно перед передачей) */
    wr32(base + S3C_SPI_CH_CFG, cfg->mode & (SPI_CPOL_HIGH | SPI_CPHA_B));

    /* 7. Предделитель тактирования:
     *    бит[8] = 1 (источник = PCLK), биты[7:0] = div
     *    freq = pclk / (2 * (div + 1)); при div=12, pclk=26MHz → ~1 MHz */
    wr32(base + S3C_SPI_CLK_CFG, (1U << 8) | (cfg->clk_div & 0xFFU));

    /* 8. 8-битный режим передачи (MODE_CFG = 0 по умолчанию = 8-bit) */
    wr32(base + S3C_SPI_MODE_CFG, 0);

    /* 9. Выравнивание байтов: без swap */
    wr32(base + S3C_SPI_SWAP_CFG, 0);

    /* 10. Задержка feedback-clock: 0 (DTS: samsung,spi-feedback-delay = <0>) */
    wr32(base + S3C_SPI_FB_CLK_SEL, 0);

    /* 11. CS: ручной режим (CS ведёт GPIO, не аппаратный CS пина) */
    wr32(base + S3C_SPI_SLAVE_SEL, 0);

    ramoops_puts("[spi11] init done: CH_CFG="); ramoops_put_hex(rd32(base + S3C_SPI_CH_CFG));
    ramoops_puts(" CLK_CFG="); ramoops_put_hex(rd32(base + S3C_SPI_CLK_CFG));
    ramoops_puts(" STATUS="); ramoops_put_hex(rd32(base + S3C_SPI_STATUS));
    ramoops_puts("\n");
}

void spi_gs101_flush(const struct spi_gs101_cfg *cfg) {
    uint64_t base = cfg->base;
    /* Сохраняем конфигурационные регистры */
    uint32_t ch   = rd32(base + S3C_SPI_CH_CFG);
    uint32_t mode = rd32(base + S3C_SPI_MODE_CFG);
    uint32_t clk  = rd32(base + S3C_SPI_CLK_CFG);
    uint32_t fb   = rd32(base + S3C_SPI_FB_CLK_SEL);

    /* Software reset сбрасывает FIFO, не трогая конфиг (если его держать) */
    wr32(base + S3C_SPI_CH_CFG, ch | SPI_CH_SW_RST);
    nop_delay_spi(50);
    wr32(base + S3C_SPI_CH_CFG, ch & ~SPI_CH_SW_RST);

    /* Восстановление */
    wr32(base + S3C_SPI_MODE_CFG, mode);
    wr32(base + S3C_SPI_CLK_CFG, clk);
    wr32(base + S3C_SPI_FB_CLK_SEL, fb);
    nop_delay_spi(20);
}

/* ── Передача одного чанка (≤ SPI_GS101_CHUNK_SIZE байт) ─────────────── */
/* Порядок действий взят из s3c64xx_enable_datapath() (spi-s3c64xx.c строки ~565–610):
 *   1. PACKET_CNT_EN снять
 *   2. Записать счётчик
 *   3. Поднять PACKET_CNT_EN
 *   4. Заполнить TX FIFO
 *   5. Поднять CH_CFG c TXCH_ON | RXCH_ON
 *   6. Ждать RX_LVL == n
 *   7. Слить RX FIFO
 *   8. Выключить TX/RX в CH_CFG
 */
static int spi_xfer_chunk(const struct spi_gs101_cfg *cfg,
                          const uint8_t *tx, uint8_t *rx, int n) {
    uint64_t  base  = cfg->base;
    uint32_t  mode  = cfg->mode & (SPI_CPOL_HIGH | SPI_CPHA_B);
    uint32_t  to    = SPI_GS101_RX_TIMEOUT;

    spi_gs101_flush(cfg);

    /* Задать счётчик пакетов */
    wr32(base + S3C_SPI_PACKET_CNT, 0);
    wr32(base + S3C_SPI_PACKET_CNT, (uint32_t)n);
    wr32(base + S3C_SPI_PACKET_CNT, (uint32_t)n | SPI_PKT_CNT_EN);

    /* Заполнить TX FIFO перед включением TX */
    for (int i = 0; i < n; i++)
        wr32(base + S3C_SPI_TX_DATA, tx ? (uint32_t)tx[i] : 0U);

    /* Запустить TX + RX */
    wr32(base + S3C_SPI_CH_CFG, mode | SPI_CH_RXCH_ON | SPI_CH_TXCH_ON);

    /* Ждать заполнения RX FIFO */
    while (SPI_RX_LVL(rd32(base + S3C_SPI_STATUS)) < (uint32_t)n && --to)
        ;

    if (!to) {
        /* Таймаут: останавливаем блок и сообщаем */
        wr32(base + S3C_SPI_CH_CFG, mode);
        wr32(base + S3C_SPI_PACKET_CNT, 0);
        ramoops_puts("[spi11] RX timeout STATUS=");
        ramoops_put_hex(rd32(base + S3C_SPI_STATUS));
        ramoops_puts(" n="); ramoops_put_hex((uint64_t)n);
        ramoops_puts("\n");
        return -1;
    }

    /* Вычитать RX FIFO */
    for (int i = 0; i < n; i++) {
        uint8_t byte = (uint8_t)(rd32(base + S3C_SPI_RX_DATA) & 0xFFU);
        if (rx)
            rx[i] = byte;
    }

    /* Выключить TX/RX */
    wr32(base + S3C_SPI_CH_CFG, mode);
    wr32(base + S3C_SPI_PACKET_CNT, 0);
    return 0;
}

int spi_gs101_transfer(const struct spi_gs101_cfg *cfg,
                       const uint8_t *tx, uint8_t *rx, int n) {
    for (int off = 0; off < n; off += SPI_GS101_CHUNK_SIZE) {
        int chunk = n - off;
        if (chunk > SPI_GS101_CHUNK_SIZE)
            chunk = SPI_GS101_CHUNK_SIZE;
        if (spi_xfer_chunk(cfg,
                           tx ? tx + off : 0,
                           rx ? rx + off : 0,
                           chunk) < 0)
            return -1;
    }
    return 0;
}
