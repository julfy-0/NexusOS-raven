/*
 * btn_power.c — Кнопки питания и громкости для Pixel 6 Pro (Google Tensor GS101)
 *
 * === Аппаратная трассировка (из DTS gs101-common.dtsi и pinctrl-gs.c) ===
 *  Кнопка   | GPIO     | Pinctrl base    | Offset | Pin # | DAT бит | CON бит
 *  ---------|----------|-----------------|--------|-------|---------|---------
 *  POWER    | GPA10[1] | ALIVE 0x174D0000| 0xE0   | 1     | 1       | [7:4]
 *  VOL DOWN | GPA7[3]  | FAR_ALIVE 0x174E0000 | 0x20 | 3  | 3       | [15:12]
 *  VOL UP   | GPA8[1]  | FAR_ALIVE 0x174E0000 | 0x40 | 1  | 1       | [7:4]
 *
 *  Все кнопки: pull-up на плате (активный НИЗКИЙ: нажата = 0, отпущена = 1).
 *
 * === Поведение с МГНОВЕННОЙ визуальной индикацией ===
 *  • POWER нажатие (короткое) → Всплывающий тост на экране (Power button detected)
 *  • POWER удержание 5 сек    → Плавная полоса обратного отсчёта на экране (50 FPS)
 *                               по истечении 5 сек → PMU PS_HOLD + PSCI SYSTEM_OFF
 *  • POWER + VOL_DOWN (комбо) → Мгновенный экран "Rebooting to Fastboot"
 *                               PMU swreset (cookie bootloader пока не подтверждён)
 */

#include "a64.h"
#include "btn_power.h"
#include "gs101_gpio.h"
#include "gs101_disp.h"
#include "disp_draw.h"
#include "disp_ui.h"
#include "ramoops_state.h"
#include "pmu_smc.h"

/* ── Банки пинов кнопок (через gs101_gpio) ───────────────────────────── */
#define GPA10_BANK          GS101_BANK(GS101_PINCTRL_ALIVE,     GPA10_OFFSET) /* POWER */
#define GPA7_BANK           GS101_BANK(GS101_PINCTRL_FAR_ALIVE, GPA7_OFFSET)  /* VOL_DOWN */
#define GPA8_BANK           GS101_BANK(GS101_PINCTRL_FAR_ALIVE, GPA8_OFFSET)  /* VOL_UP */

/* PMU — значения из gs101.dtsi (узел samsung,exynos-reboot, строки 441-447 ветки raviole android15-qpr1):
 *   pshold-control-offset = 0x3E9C, trigger 0x100   -> выключение: сбросить бит 8
 *   swreset-system-offset = 0x3A00, trigger 0x2     -> программный сброс системы
 *   reboot-cmd-offset     = 0x0810                  -> сюда Linux пишет код причины перезагрузки
 * Значение cookie для bootloader/fastboot пока НЕ подтверждено исходниками, поэтому не пишется. */
#define GS101_PMU_BASE       0x17460000ULL
#define PMU_PSHOLD_CTRL      (GS101_PMU_BASE + 0x3E9C)
#define PMU_PSHOLD_TRIGGER   0x100U
#define PMU_SWRESET_SYSTEM   (GS101_PMU_BASE + 0x3A00)
#define PMU_SWRESET_TRIGGER  0x2U
#define PMU_REBOOT_CMD       (GS101_PMU_BASE + 0x0810)

/* PSCI SMC */
#define PSCI_SYSTEM_OFF     0x84000008ULL
#define PSCI_SYSTEM_RESET   0x84000009ULL

/* ── Счётчики удержания и состояния (на базе 50 Гц тиков главного цикла) ── */
#define HOLD_TICKS_POWEROFF  250  /* 250 тиков * 20 мс = 5000 мс (5 секунд) */

static uint64_t g_power_hold_start = 0;   /* cntvct в момент нажатия, 0 = не нажата */
static uint64_t g_last_hud_draw    = 0;   /* когда последний раз рисовали HUD */
static uint32_t g_power_hold_ticks = 0;   /* "виртуальные" тики по 20 мс, считаются из реального времени */

static inline uint64_t now_ticks(void) {
    uint64_t t;
    __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(t));
    return t;
}
static inline uint64_t ms_to_ticks(uint32_t ms) {
    uint64_t f = timer_freq();
    if (!f) f = 24576000ULL;
    return (f * (uint64_t)ms) / 1000ULL;
}
static uint32_t g_last_btn_state   = 0;
static uint32_t g_toast_ticks      = 0;
static const char *g_toast_msg     = 0;

/* ── Инициализация GPIO (через gs101_gpio API) ──────────────────────── */
void btn_power_init(void) {
    /* GPA10[1] (POWER): GPIO Input (func=0) */
    gs101_gpio_cfg_pin(GPA10_BANK, 1, 0);

    /* GPA7[3] (VOL_DOWN): GPIO Input (func=0) */
    gs101_gpio_cfg_pin(GPA7_BANK, 3, 0);

    /* GPA8[1] (VOL_UP): GPIO Input (func=0) */
    gs101_gpio_cfg_pin(GPA8_BANK, 1, 0);

    ramoops_puts("[btn] buttons initialized via gs101_gpio (GPA10[1], GPA7[3], GPA8[1])\n");
}

/* ── Чтение состояния кнопок ────────────────────────────────────────── */
uint32_t btn_power_read(void) {
    uint32_t result = 0;

    /* DAT bit == 0 означает нажатие (активный низкий) */
    if (!gs101_gpio_get_val(GPA10_BANK, 1)) result |= BTN_POWER;
    if (!gs101_gpio_get_val(GPA7_BANK,  3)) result |= BTN_VOL_DOWN;
    if (!gs101_gpio_get_val(GPA8_BANK,  1)) result |= BTN_VOL_UP;

    return result;
}

uint32_t btn_power_get_raw_a10(void) { return rd32(GPA10_BANK + PIN_REG_DAT); }
uint32_t btn_power_get_raw_a7(void)  { return rd32(GPA7_BANK  + PIN_REG_DAT); }
uint32_t btn_power_get_raw_a8(void)  { return rd32(GPA8_BANK  + PIN_REG_DAT); }

/* ── Выключение устройства ──────────────────────────────────────────── */
static uint64_t psci_off_call(void) {
    register uint64_t x0 __asm__("x0") = PSCI_SYSTEM_OFF;
    __asm__ volatile("smc #0" : "+r"(x0) :: "x1", "x2", "x3", "memory");
    return x0;
}

static void hex8(char *dst, uint32_t v) {
    for (int i = 0; i < 8; i++) {
        uint32_t n = (v >> ((7 - i) * 4)) & 0xF;
        dst[i] = (char)(n < 10 ? '0' + n : 'A' + n - 10);
    }
}

static void do_poweroff(struct disp_state *ds) {
    ramoops_puts("[btn] POWER OFF: PMU PS_HOLD, затем PSCI SYSTEM_OFF\n");
    ramoops_flush();

    if (ds && ds->fb) {
        int h = (int)ds->height;
        /* Экран выключения */
        disp_clear(ds, 0x050408);
        disp_draw_text_center(ds, "POWERING OFF...", h / 2 - 20, 3, 0xF43F5E);
        disp_draw_text_center(ds, "Pixel 6 Pro  -  NexusOS Shutdown", h / 2 + 40, 2, 0xD0C8E0);
        disp_present(ds);
    }

    delay_ms(150);

    /* 1) PMU PS_HOLD: сбрасываем бит 8 в PMU+0x3E9C (как exynos-reboot по DTS).
     * 2) Если питание осталось — PSCI SYSTEM_OFF. Если вернулись — не сработало
     *    (USB/зарядка могут держать питание): показываем коды и повторяем раз в секунду. */
    for (;;) {
        uint32_t before = rd32(PMU_PSHOLD_CTRL);
        /* Запись PS_HOLD через SMC (PMU_ALIVE защищён, пишет EL3; по mainline exynos-pmu —
         * не проверено на железе). 0x3E9C <= 0x3FFF -> atomic clear бита 8. */
        uint64_t rc1 = pmu_smc_clear_bit(0x3E9C, 8);
        ramoops_puts("[btn] smc clear_bit(3E9C,8) rc="); ramoops_put_hex(rc1); ramoops_puts("\n");
        ramoops_flush();
        delay_ms(300);
        uint64_t rc2 = 0;
        if (rd32(PMU_PSHOLD_CTRL) & PMU_PSHOLD_TRIGGER) {
            rc2 = pmu_smc_rmw(0x3E9C, PMU_PSHOLD_TRIGGER, 0);
            ramoops_puts("[btn] smc rmw(3E9C,mask100,0) rc="); ramoops_put_hex(rc2); ramoops_puts("\n");
            ramoops_flush();
            delay_ms(300);
        }
        uint32_t after = rd32(PMU_PSHOLD_CTRL);
        ramoops_puts("[btn] PS_HOLD before="); ramoops_put_hex(before);
        ramoops_puts(" after="); ramoops_put_hex(after); ramoops_puts("\n");
        ramoops_flush();

        uint64_t r = psci_off_call();
        ramoops_puts("[btn] SYSTEM_OFF returned x0="); ramoops_put_hex(r); ramoops_puts("\n");
        ramoops_flush();

        if (ds && ds->fb) {
            int h = (int)ds->height;
            char m1[32] = "PSHOLD ........ > ........";
            char m2[32] = "SMC ret=0x........";
            hex8(m1 + 7, before); hex8(m1 + 18, after);
            hex8(m2 + 10, (uint32_t)r);
            disp_fill_rect(ds, 0, h / 2 + 90, (int)ds->width, 220, 0x050408);
            disp_draw_text_center(ds, "POWER OFF FAILED", h / 2 + 100, 2, 0xF43F5E);
            disp_draw_text_center(ds, m1, h / 2 + 150, 2, 0xFBBF24);
            disp_draw_text_center(ds, m2, h / 2 + 200, 2, 0xFBBF24);
            disp_draw_text_center(ds, "unplug USB cable", h / 2 + 250, 2, 0xD0C8E0);
            disp_present(ds);
        }
        delay_ms(1000);
    }
}

/* ── Перезагрузка в Fastboot ────────────────────────────────────────── */
static void do_reboot_fastboot(struct disp_state *ds) {
    ramoops_puts("[btn] FASTBOOT reboot combo triggered\n");
    ramoops_flush();

    if (ds && ds->fb) {
        int h = (int)ds->height;
        disp_clear(ds, 0x080A18);
        disp_draw_text_center(ds, "REBOOTING TO FASTBOOT...", h / 2 - 20, 3, 0x38BDF8);
        disp_draw_text_center(ds, "Entering Bootloader Mode", h / 2 + 40, 2, 0xFBBF24);
        disp_present(ds);
    }

    /* Cookie bootloader в PMU+0x0810 не пишем: значение не подтверждено исходниками. */
    ramoops_puts("[btn] PMU reboot-cmd(0x810)="); ramoops_put_hex(rd32(PMU_REBOOT_CMD)); ramoops_puts("\n");

    delay_ms(150);

    /* 1) PMU swreset (PMU+0x3A00 <- 0x2), 2) если не сработало — PSCI SYSTEM_RESET. */
    uint64_t rcs = pmu_smc_write(0x3A00, PMU_SWRESET_TRIGGER);
    ramoops_puts("[btn] smc swreset rc="); ramoops_put_hex(rcs); ramoops_puts("\n");
    ramoops_flush();
    delay_ms(500);
    ramoops_puts("[btn] swreset did not reset, trying PSCI\n");
    register uint64_t x0 __asm__("x0") = PSCI_SYSTEM_RESET;
    __asm__ volatile("smc #0" : "+r"(x0) :: "x1", "x2", "x3", "memory");
    ramoops_puts("[btn] SYSTEM_RESET returned x0="); ramoops_put_hex(x0); ramoops_puts("\n");
    ramoops_flush();
    for (;;) __asm__ volatile("wfe");
}

/* ── Отрисовка Power HUD (индикатор удержания питания) ────────────────── */
static void draw_power_hud(struct disp_state *ds, uint32_t hold_ticks) {
    if (!ds || !ds->fb) return;

    int w = (int)ds->width;
    int hud_w = w - 160;
    int hud_h = 160;
    int hud_x = 80;
    int hud_y = 200;

    /* Плашка модального окна удержания кнопки питания */
    disp_rounded_rect(ds, hud_x, hud_y, hud_w, hud_h, 24, 0x160F22, 245);
    disp_rect_outline(ds, hud_x, hud_y, hud_w, hud_h, 3, 0xF43F5E);

    /* Текст обратного отсчёта */
    uint32_t remain_ms = (hold_ticks < HOLD_TICKS_POWEROFF) ? 
                         (HOLD_TICKS_POWEROFF - hold_ticks) * 20 : 0;
    uint32_t sec_int  = remain_ms / 1000;
    uint32_t sec_frac = (remain_ms % 1000) / 100;

    char msg[64] = "Hold Power to Turn Off:  . s";
    msg[24] = '0' + (char)sec_int;
    msg[26] = '0' + (char)sec_frac;
    disp_draw_text(ds, msg, hud_x + 36, hud_y + 30, 2, 0xF3EFF8);

    /* Полоса прогресса */
    int bar_x = hud_x + 36;
    int bar_y = hud_y + 85;
    int bar_w = hud_w - 72;
    int bar_h = 24;

    disp_rounded_rect(ds, bar_x, bar_y, bar_w, bar_h, 10, 0x2A1C3C, 255);
    int fill_w = (int)((uint64_t)hold_ticks * (uint64_t)bar_w / HOLD_TICKS_POWEROFF);
    if (fill_w > bar_w) fill_w = bar_w;
    if (fill_w > 0) {
        disp_rounded_rect(ds, bar_x, bar_y, fill_w, bar_h, 10, 0xF43F5E, 255);
    }
}

/* ── Опрос кнопок из главного цикла ──────────────────────────────────── */
void btn_power_check(struct disp_state *ds) {
    uint32_t btns = btn_power_read();
    uint64_t now  = now_ticks();

    /* 1. Комбинация: POWER + VOL_DOWN → немедленный Fastboot Reboot */
    if ((btns & BTN_POWER) && (btns & BTN_VOL_DOWN)) {
        do_reboot_fastboot(ds);
        return;
    }

    /* 2. Удержание POWER → выключение через 5 секунд РЕАЛЬНОГО времени */
    if (btns & BTN_POWER) {
        if (g_power_hold_start == 0) {
            g_power_hold_start = now;
            g_last_hud_draw = 0;
        }
        uint64_t held = now - g_power_hold_start;
        uint64_t full = ms_to_ticks(HOLD_TICKS_POWEROFF * 20);   /* 5000 мс */

        /* переводим в "тики по 20 мс" для HUD */
        g_power_hold_ticks = (uint32_t)(held * HOLD_TICKS_POWEROFF / full);
        if (g_power_hold_ticks > HOLD_TICKS_POWEROFF) g_power_hold_ticks = HOLD_TICKS_POWEROFF;

        /* Рисуем HUD не чаще ~10 раз в секунду, иначе перерисовка 1440x3120 тормозит цикл */
        if (ds && ds->fb && (g_last_hud_draw == 0 || now - g_last_hud_draw >= ms_to_ticks(100))) {
            draw_power_hud(ds, g_power_hold_ticks);
            disp_present(ds);
            g_last_hud_draw = now;
        }

        if (held >= full) {
            do_poweroff(ds);
            return;
        }
    } else if (g_power_hold_start != 0) {
        /* Кнопка питания отпущена */
        uint64_t held = now - g_power_hold_start;
        g_power_hold_start = 0;
        g_power_hold_ticks = 0;

        if (held >= ms_to_ticks(100)) {
            g_toast_ticks = 60;
            g_toast_msg   = "Power key tapped (hold 5s to power off)";
        }
        if (ds && ds->fb) {
            disp_ui_draw(ds, 0);
            disp_present(ds);
        }
    }

    /* 3. Одиночные нажатия громкости */
    if ((btns & BTN_VOL_DOWN) && !(g_last_btn_state & BTN_VOL_DOWN)) {
        g_toast_ticks = 50;
        g_toast_msg   = "Volume Down: -";
    }
    if ((btns & BTN_VOL_UP) && !(g_last_btn_state & BTN_VOL_UP)) {
        g_toast_ticks = 50;
        g_toast_msg   = "Volume Up: +";
    }

    /* Отрисовка тост-сообщения при нажатии */
    if (g_toast_ticks > 0 && ds && ds->fb) {
        g_toast_ticks--;
        if (g_toast_msg) {
            int tw = disp_text_width(g_toast_msg, 2);
            int tx = ((int)ds->width - tw - 48) / 2;
            int ty = (int)ds->height - 400;
            disp_rounded_rect(ds, tx, ty, tw + 48, 56, 28, 0x181008, 220);
            disp_draw_text(ds, g_toast_msg, tx + 24, ty + 12, 2, 0xFFFFFF);
            disp_present(ds);
        }
    }

    g_last_btn_state = btns;
}