/*
 * btn_power.h — Кнопки питания/громкости для Pixel 6 Pro (gs101)
 *
 * Кнопки (из arch/arm64/boot/dts/google/gs101-common.dtsi):
 *   POWER    : GPA10[1] — ALIVE pinctrl @ 0x174D0000, offset 0xE0, бит 1
 *   VOL DOWN : GPA7[3]  — FAR_ALIVE pinctrl @ 0x174E0000, offset 0x20, бит 3
 *   VOL UP   : GPA8[1]  — FAR_ALIVE pinctrl @ 0x174E0000, offset 0x40, бит 1
 *
 * Все кнопки активный низкий (pull-up на плате).
 */

#ifndef NEXUS_BTN_POWER_H
#define NEXUS_BTN_POWER_H

#include <stdint.h>
#include "gs101_disp.h"
#include "nexus_version.h"
#define BTN_POWER_VERSION NEXUS_VERSION_STRING

/* Инициализировать GPIO кнопок как Input */
void btn_power_init(void);

/* Опросить кнопки. Возвращает битовую маску: */
#define BTN_POWER    (1U << 0)
#define BTN_VOL_DOWN (1U << 1)
#define BTN_VOL_UP   (1U << 2)
uint32_t btn_power_read(void);

/* Чтение сырых регистров GPIO DAT для отладки */
uint32_t btn_power_get_raw_a10(void);
uint32_t btn_power_get_raw_a7(void);
uint32_t btn_power_get_raw_a8(void);

/*
 * Политики нажатий — вызывать из главного цикла:
 *   - POWER удержание 5 сек → выключение
 *   - POWER + VOL_DOWN → перезагрузка в fastboot
 *   - Выводит Power HUD прямо на дисплей
 */
void btn_power_check(struct disp_state *ds);

#endif
