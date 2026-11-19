/*
 * gs101_gpio.c — NexusOS GPIO / Pinctrl API для Google Tensor GS101 (Pixel 6 Pro)
 *
 * Реализация извлечена и адаптирована из:
 *   vendor/stock-drivers/drivers/pinctrl/samsung/pinctrl-samsung.c
 *   vendor/stock-drivers/drivers/pinctrl/samsung/pinctrl-gs.c
 *
 * Принципы работы на bare-metal (freestanding):
 *   - Нет ioremap, нет спинлоков, нет PM runtime — прямые MMIO read/write.
 *   - Операции чтение-модификация-запись (RMW) через 4-битные поля на пин.
 *   - Любое обращение к регистру обесточенного домена даёт SError.
 *     Убедись, что домен питания блока включён через PMU до вызова этих функций.
 */
#include "gs101_gpio.h"
#include "gs101_disp.h"  /* rd32() / wr32() — inline helper из дисплейного заголовка */

/* ── Внутренние утилиты ───────────────────────────────────────────────── */

/* Вычислить маску и смещение для 4-битного поля пина N */
static inline void pin_field(uint32_t pin, uint32_t *shift, uint32_t *mask) {
    *shift = pin * 4U;
    *mask  = 0xFU << *shift;
}

/* ── Публичный API ────────────────────────────────────────────────────── */

void gs101_gpio_cfg_pin(uint64_t bank_base, uint32_t pin, uint32_t func) {
    uint32_t shift, mask;
    pin_field(pin, &shift, &mask);
    uint32_t val = rd32(bank_base + PIN_REG_CON);
    val = (val & ~mask) | ((func & 0xFU) << shift);
    wr32(bank_base + PIN_REG_CON, val);
}

void gs101_gpio_set_pull(uint64_t bank_base, uint32_t pin, uint32_t pull) {
    uint32_t shift, mask;
    pin_field(pin, &shift, &mask);
    uint32_t val = rd32(bank_base + PIN_REG_PUD);
    val = (val & ~mask) | ((pull & 0xFU) << shift);
    wr32(bank_base + PIN_REG_PUD, val);
}

void gs101_gpio_set_drv(uint64_t bank_base, uint32_t pin, uint32_t strength) {
    uint32_t shift, mask;
    pin_field(pin, &shift, &mask);
    uint32_t val = rd32(bank_base + PIN_REG_DRV);
    val = (val & ~mask) | ((strength & 0xFU) << shift);
    wr32(bank_base + PIN_REG_DRV, val);
}

void gs101_gpio_set_val(uint64_t bank_base, uint32_t pin, int val) {
    uint32_t reg = rd32(bank_base + PIN_REG_DAT);
    if (val)
        reg |= (1U << pin);
    else
        reg &= ~(1U << pin);
    wr32(bank_base + PIN_REG_DAT, reg);
}

int gs101_gpio_get_val(uint64_t bank_base, uint32_t pin) {
    return (int)((rd32(bank_base + PIN_REG_DAT) >> pin) & 1U);
}

void gs101_gpio_cfg_bank_mask(uint64_t bank_base, uint32_t pin_mask, uint32_t func) {
    uint32_t con = rd32(bank_base + PIN_REG_CON);
    for (uint32_t pin = 0; pin < 8U; pin++) {
        if (!(pin_mask & (1U << pin))) continue;
        uint32_t shift = pin * 4U;
        uint32_t mask  = 0xFU << shift;
        con = (con & ~mask) | ((func & 0xFU) << shift);
    }
    wr32(bank_base + PIN_REG_CON, con);
}
