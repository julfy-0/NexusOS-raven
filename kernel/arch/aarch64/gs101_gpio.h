/*
 * gs101_gpio.h — NexusOS GPIO / Pinctrl API для Google Tensor GS101 (Pixel 6 Pro)
 *
 * Источник: vendor/stock-drivers/drivers/pinctrl/samsung/pinctrl-gs.c,
 *           pinctrl-samsung.h, pinctrl-exynos.h
 *           arch/arm64/boot/dts/google/gs101-pinctrl.dtsi
 *
 * Архитектура: каждый банк пинов GS101 имеет 4 регистра с 4 битами на пин:
 *   CON (направление/функция)  DAT (уровень)  PUD (подтяжка)  DRV (сила тока)
 */
#ifndef NEXUS_GS101_GPIO_H
#define NEXUS_GS101_GPIO_H

#include "a64.h"

/* ── Базовые адреса контроллеров Pinctrl (из gs101-pinctrl.dtsi) ──────── */
#define GS101_PINCTRL_ALIVE    0x174D0000ULL  /* ALIVE — питается всегда */
#define GS101_PINCTRL_FAR_ALIVE 0x174E0000ULL /* FAR_ALIVE — кнопки громкости и др. */
#define GS101_PINCTRL_PERIC0   0x10840000ULL  /* PERIC0 — SPI/I2C/UART (группа 0) */
#define GS101_PINCTRL_PERIC1   0x10C40000ULL  /* PERIC1 — SPI/I2C/UART (группа 1) */
#define GS101_PINCTRL_HSI0     0x10440000ULL  /* HSI0 — USB/PCIE */
#define GS101_PINCTRL_HSI2     0x14440000ULL  /* HSI2 — UFS/eMMC */

/* ── Смещения регистров банка пинов (pinctrl-samsung.h) ───────────────── */
#define PIN_REG_CON    0x00  /* функция: 0=Input, 1=Output, 2..F=альтернатив. функция */
#define PIN_REG_DAT    0x04  /* данные: бит N = пин N (read-write) */
#define PIN_REG_PUD    0x08  /* подтяжка: 4 бита/пин → 0=Off, 1=Down, 3=Up */
#define PIN_REG_DRV    0x0C  /* сила тока: 4 бита/пин → 1X/2X/3X/4X */

/* ── Коды подтяжки (pinctrl-exynos.h, GS101-вариант) ─────────────────── */
#define GPIO_PULL_NONE  0U
#define GPIO_PULL_DOWN  1U
#define GPIO_PULL_UP    3U

/* ── Смещения конкретных банков в PERIC1 (из gs101-pinctrl.dtsi) ──────── */
/* Используются в SPI11 / тачскрин */
#define GPP23_OFFSET   0x60   /* SPI11: CLK/MOSI/MISO/CS */
#define GPP26_OFFSET   0xC0   /* TS_RESET_L / TS_SPI_SEL */

/* ── Смещения конкретных банков в ALIVE ───────────────────────────────── */
#define GPA9_OFFSET    0xC0   /* TS_INT_R2_L (прерывание тачскрина) */
#define GPA10_OFFSET   0xE0   /* POWER KEY */

/* ── Смещения конкретных банков в FAR_ALIVE ───────────────────────────── */
#define GPA7_OFFSET    0x20   /* VOL_DOWN */
#define GPA8_OFFSET    0x40   /* VOL_UP */

/* ── Вспомогательные макросы адресов ─────────────────────────────────── */
#define GS101_BANK(ctrl, offset) ((ctrl) + (uint64_t)(offset))

/* ── API ──────────────────────────────────────────────────────────────── */

/*
 * gs101_gpio_cfg_pin — установить функцию пина (CON-регистр)
 *   bank_base  : адрес банка (например GS101_PINCTRL_PERIC1 + GPP23_OFFSET)
 *   pin        : номер пина внутри банка (0..7 обычно)
 *   func       : 0=Input, 1=Output, 2..F=альтернативная функция (SPI, I2C, …)
 */
void gs101_gpio_cfg_pin(uint64_t bank_base, uint32_t pin, uint32_t func);

/*
 * gs101_gpio_set_pull — установить подтяжку пина (PUD-регистр)
 *   pull       : GPIO_PULL_NONE / GPIO_PULL_DOWN / GPIO_PULL_UP
 */
void gs101_gpio_set_pull(uint64_t bank_base, uint32_t pin, uint32_t pull);

/*
 * gs101_gpio_set_drv — установить силу тока пина (DRV-регистр)
 *   strength   : 0..3 → 1X, 2X, 3X, 4X
 */
void gs101_gpio_set_drv(uint64_t bank_base, uint32_t pin, uint32_t strength);

/*
 * gs101_gpio_set_val — выставить выходной уровень (DAT-регистр, только Output)
 */
void gs101_gpio_set_val(uint64_t bank_base, uint32_t pin, int val);

/*
 * gs101_gpio_get_val — прочитать текущий уровень пина (DAT-регистр)
 */
int  gs101_gpio_get_val(uint64_t bank_base, uint32_t pin);

/*
 * gs101_gpio_cfg_bank_mask — настроить функцию нескольких пинов за раз
 *   pin_mask : битовая маска пинов (бит N = пин N)
 *   func     : единая функция для всех пинов в маске
 */
void gs101_gpio_cfg_bank_mask(uint64_t bank_base, uint32_t pin_mask, uint32_t func);

#endif /* NEXUS_GS101_GPIO_H */
