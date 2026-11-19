/*
 * disp_ui.c — NexusOS Mobile Desktop & Debug Suite для Google Pixel 6 Pro (gs101)
 *
 * Создан строго по макету пользователя:
 *   • Обои из ассетов: golden spiral rose (wall3.png)
 *   • Верхняя строка состояния: время в капсуле, вырез камеры, статус батареи
 *   • Верхняя сетка 2×4: белые скругленные иконки в стиле iOS
 *   • По центру: поисковая строка «Search»
 *   • Нижний Dock: полупрозрачное затемнённое стекло с 4 иконками
 *   • Домашняя полоска (Home Pill) в самом низу
 *
 * Встроенные приложения для отладки:
 *   1. Terminal (OSK + интерактивный шелл)
 *   2. Touch DBG (мониторинг регистров SPI11 и пакетов FTM5)
 *   3. Touch Test (холст для рисования и проверка калибровки)
 *   4. Kernel Log (живой dmesg из Ramoops со скроллом)
 *   5. System Diag (диагностика Tensor GS101, DECON0, RAM, GPIO кнопок)
 *   6. Settings (параметры AMOLED, 120Hz)
 *   7. Files (RAM-VFS файловый менеджер)
 *   8. Reboot / Fastboot
 */

#include "a64.h"
#include "ramoops_state.h"
#include "gs101_disp.h"
#include "disp_draw.h"
#include "disp_ui.h"
#include "disp_osk.h"
#include "touch_raven.h"
#include "usb_gs101.h"
#include "btn_power.h"
#include "wallpaper_wall3.h"

/* ── Цветовая палитра ─────────────────────────────────────────────────── */
#define C_WHITE       0xFFFFFF
#define C_GOLD        0xE8B84B
#define C_GOLD_DARK   0x6B4F1A
#define C_DARK_BG     0x0E0B12
#define C_BORDER      0x382A1A
#define C_BORDER_HI   0x7A5E2E
#define C_ACCENT      0x38BDF8
#define C_EMERALD     0x34D399
#define C_ROSE        0xF43F5E
#define C_AMBER       0xFBBF24
#define C_PINK        0xFF6B9D
#define C_MUTED       0xD4AF37
#define C_DIM         0x9E844D
#define C_TERM_FG     0x34D399
#define C_TERM_CMD    0x38BDF8

/* ── Разрешение тач-панели FTM5 (DTS gs101-slider-touch.dtsi) ─────────── */
#define TOUCH_PANEL_MAX_X 1440
#define TOUCH_PANEL_MAX_Y 3040

/* ── Идентификаторы видов экрана ─────────────────────────────────────── */
#define VIEW_DESKTOP    0
#define VIEW_TERMINAL   1
#define VIEW_SETTINGS   2
#define VIEW_FILES      3
#define VIEW_DIAG       4
#define VIEW_TOUCH_DBG  5
#define VIEW_TOUCH_TEST 6
#define VIEW_LOGS       7

/* ── Состояние мобильного UI ─────────────────────────────────────────── */
static int      g_view           = VIEW_DESKTOP;
static uint32_t g_uptime_sec     = 0;
static int      g_last_tap_x     = 720;
static int      g_last_tap_y     = 1560;
static int      g_touch_detected = 0;

/* Терминал */
static char g_term_history[8][96];
static int  g_term_history_count = 0;
static char g_term_output[96]    = "Welcome to NexusOS Mobile Terminal";

/* Настройки */
static int g_settings_sel = 0;

/* Логи ядра (скролл) */
static int g_log_scroll_offset = 0;

/* Холст теста тача */
#define CANVAS_MAX_POINTS 128
static int16_t g_canvas_x[CANVAS_MAX_POINTS];
static int16_t g_canvas_y[CANVAS_MAX_POINTS];
static uint8_t g_canvas_color[CANVAS_MAX_POINTS];
static int     g_canvas_count = 0;
static int     g_canvas_head  = 0;

/* Буфер истории касаний */
#define TOUCH_HIST_LEN 64
static uint16_t g_th_x[TOUCH_HIST_LEN];
static uint16_t g_th_y[TOUCH_HIST_LEN];
static uint8_t  g_th_p[TOUCH_HIST_LEN];
static int      g_th_head  = 0;
static int      g_th_count = 0;

static void touch_hist_push(uint16_t x, uint16_t y, uint8_t pressed) {
    g_th_x[g_th_head] = x;
    g_th_y[g_th_head] = y;
    g_th_p[g_th_head] = pressed;
    g_th_head = (g_th_head + 1) % TOUCH_HIST_LEN;
    if (g_th_count < TOUCH_HIST_LEN) g_th_count++;
}

/* ── Форматирование строк ────────────────────────────────────────────── */
static int fmt_uint(uint32_t v, char *buf) {
    if (v == 0) { buf[0] = '0'; buf[1] = '\0'; return 1; }
    char tmp[12]; int n = 0;
    while (v > 0) { tmp[n++] = '0' + (char)(v % 10); v /= 10; }
    for (int i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    buf[n] = '\0';
    return n;
}

static void fmt_hhmm(uint32_t sec, char *buf) {
    uint32_t h = (sec / 3600) % 24;
    uint32_t m = (sec / 60) % 60;
    buf[0] = '0' + (char)(h / 10);
    buf[1] = '0' + (char)(h % 10);
    buf[2] = ':';
    buf[3] = '0' + (char)(m / 10);
    buf[4] = '0' + (char)(m % 10);
    buf[5] = '\0';
}

static int slen(const char *s) {
    int n = 0; while (s && s[n]) n++; return n;
}

static void scopy(char *dst, const char *src) {
    if (!dst || !src) return;
    while (*src) *dst++ = *src++;
    *dst = '\0';
}

static void fmt_hex8(uint8_t v, char *buf) {
    static const char h[] = "0123456789ABCDEF";
    buf[0] = '0'; buf[1] = 'x';
    buf[2] = h[v >> 4]; buf[3] = h[v & 0xF];
    buf[4] = '\0';
}

static void fmt_hex32(uint32_t v, char *buf) {
    static const char h[] = "0123456789ABCDEF";
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 7; i >= 2; i--) { buf[i] = h[v & 0xF]; v >>= 4; }
    buf[8] = '\0';
}

static int ui_scale(struct disp_state *s) {
    return (s->width >= 1200) ? 3 : ((s->width >= 800) ? 2 : 1);
}

/* ── 1. Отрисовка фотореалистичных обоев из ассетов (wall3.png) ──────── */
static void draw_wallpaper(struct disp_state *s) {
    if (!s || !s->fb) return;

    uint32_t words = s->pitch / 4u;
    uint32_t w = s->width;
    uint32_t h = s->height;

    /*
     * Быстрый рендер 360×760 → 1440×3040 с масштабированием 4×4 без делений.
     * Если разрешение отличается — используется быстрый fixed-point step.
     */
    if (w == 1440 && h == 3040) {
        for (uint32_t y = 0; y < h; y++) {
            uint32_t sy = y >> 2;
            if (sy >= WALLPAPER_H) sy = WALLPAPER_H - 1;
            const uint16_t *src_row = g_wallpaper_wall3 + sy * WALLPAPER_W;
            volatile uint32_t *dst_row = s->fb + (uint64_t)y * words;

            for (uint32_t x = 0; x < w; x++) {
                uint32_t sx = x >> 2;
                if (sx >= WALLPAPER_W) sx = WALLPAPER_W - 1;
                uint16_t px = src_row[sx];
                uint8_t r = (uint8_t)((px >> 11) << 3);
                uint8_t g = (uint8_t)(((px >> 5) & 0x3F) << 2);
                uint8_t b = (uint8_t)((px & 0x1F) << 3);
                dst_row[x] = disp_pack(s->fmt, r, g, b);
            }
        }
    } else {
        uint32_t x_step = (WALLPAPER_W << 16) / (w > 0 ? w : 1);
        uint32_t y_step = (WALLPAPER_H << 16) / (h > 0 ? h : 1);

        for (uint32_t y = 0; y < h; y++) {
            uint32_t sy = (y * y_step) >> 16;
            if (sy >= WALLPAPER_H) sy = WALLPAPER_H - 1;
            const uint16_t *src_row = g_wallpaper_wall3 + sy * WALLPAPER_W;
            volatile uint32_t *dst_row = s->fb + (uint64_t)y * words;
            uint32_t x_acc = 0;

            for (uint32_t x = 0; x < w; x++) {
                uint32_t sx = x_acc >> 16;
                x_acc += x_step;
                if (sx >= WALLPAPER_W) sx = WALLPAPER_W - 1;
                uint16_t px = src_row[sx];
                uint8_t r = (uint8_t)((px >> 11) << 3);
                uint8_t g = (uint8_t)(((px >> 5) & 0x3F) << 2);
                uint8_t b = (uint8_t)((px & 0x1F) << 3);
                dst_row[x] = disp_pack(s->fmt, r, g, b);
            }
        }
    }
}

/* ── 2. Верхняя строка состояния (Status Bar в точности как на макете) ── */
static void draw_status_bar(struct disp_state *s) {
    int w = (int)s->width;

    /* Вырез фронтальной камеры Pixel 6 Pro (X=720, Y=75, R=45) */
    disp_circle(s, w / 2, 75, 46, 0x010103, 255);

    /* 1. Слева: Время в скруглённой капсуле-пилюле (как на макете) */
    int time_pill_x = 64;
    int time_pill_y = 52;
    int time_pill_w = 150;
    int time_pill_h = 56;
    disp_rounded_rect(s, time_pill_x, time_pill_y, time_pill_w, time_pill_h, 28, 0x000000, 75);

    char tbuf[8]; fmt_hhmm(g_uptime_sec, tbuf);
    int tw = disp_text_width(tbuf, 2);
    disp_draw_text(s, tbuf, time_pill_x + (time_pill_w - tw) / 2, time_pill_y + 12, 2, C_WHITE);

    /* 2. Справа: Индикатор батареи (горизонтальная капсула как на макете) */
    int bat_w = 72;
    int bat_h = 36;
    int bat_x = w - 64 - bat_w;
    int bat_y = 62;

    /* Внешний контур батареи */
    disp_rounded_rect(s, bat_x, bat_y, bat_w, bat_h, 12, 0x000000, 70);
    disp_rect_outline(s, bat_x, bat_y, bat_w, bat_h, 2, C_WHITE);
    /* Наконечник батареи справа */
    disp_rounded_rect(s, bat_x + bat_w, bat_y + 10, 6, 16, 3, C_WHITE, 255);
    /* Заполнение батареи (100% зелёно-золотой уровень) */
    disp_rounded_rect(s, bat_x + 5, bat_y + 5, bat_w - 10, bat_h - 10, 8, 0xE5C048, 255);

    /* Индикатор USB (если подключен кабель) */
    if (usb_gs101_is_connected()) {
        int usb_x = bat_x - 120;
        disp_rounded_rect(s, usb_x, bat_y - 2, 100, bat_h + 4, 14, 0x000000, 80);
        disp_draw_text(s, "USB", usb_x + 22, bat_y + 6, 2, C_EMERALD);
    }
}

/* ── 3. Мобильный заголовок окна для под-экранов с кнопкой «Назад» ────── */
static void draw_mobile_header(struct disp_state *s, const char *title, uint32_t accent) {
    int scale   = ui_scale(s);
    int sb_h    = 140;
    int head_h  = (s->height >= 2500) ? 120 : 64;
    int w       = (int)s->width;
    int cy      = sb_h + head_h / 2;

    disp_fill_rect_alpha(s, 0, sb_h, w, head_h, 0x1A1408, 230);
    disp_hline(s, 0, sb_h + head_h - 1, w, C_BORDER_HI);
    disp_fill_rect_alpha(s, 0, sb_h, w, 3, accent, 200);

    /* Кнопка «< Home» */
    int btn_w = disp_text_width("< Home", scale) + 40;
    int btn_h = head_h - 24;
    int btn_y = sb_h + 12;
    disp_rounded_rect(s, 32, btn_y, btn_w, btn_h, 14, 0x302410, 240);
    disp_rect_outline(s, 32, btn_y, btn_w, btn_h, 2, accent);
    disp_draw_text(s, "< Home", 52, cy - DISP_FONT_HEIGHT * scale / 2, scale, C_WHITE);

    /* Заголовок по центру */
    int tw = disp_text_width(title, scale);
    disp_draw_text(s, title, (w - tw) / 2, cy - DISP_FONT_HEIGHT * scale / 2, scale, C_WHITE);
}

/* ── 4. Данные приложений (8 основных + 4 в доке) ─────────────────────── */
struct mobile_app {
    const char *title;
    const char *glyph;
    uint32_t    glyph_col;
    uint32_t    tile_bg;
    int         target_view;
};

static const struct mobile_app g_grid_apps[8] = {
    { "Terminal",   ">_", C_DARK_BG,   0xFFFFFF, VIEW_TERMINAL   },
    { "Touch DBG",  "T",  0xE82664,    0xFFFFFF, VIEW_TOUCH_DBG  },
    { "Touch Test", "P",  0x0F8F60,    0xFFFFFF, VIEW_TOUCH_TEST },
    { "Kernel Log", "L",  0xC87810,    0xFFFFFF, VIEW_LOGS       },
    { "System Diag","D",  0x3858D0,    0xFFFFFF, VIEW_DIAG       },
    { "Settings",   "*",  0x7830B0,    0xFFFFFF, VIEW_SETTINGS   },
    { "Files",      "F",  0x208838,    0xFFFFFF, VIEW_FILES      },
    { "Reboot",     "R",  0xD82038,    0xFFFFFF, -1              }
};

static const struct mobile_app g_dock_apps[4] = {
    { "Terminal", ">_", 0xFFFFFF, 0x1E1E22, VIEW_TERMINAL },
    { "Files",    "F",  0xFFFFFF, 0x2A7C38, VIEW_FILES    },
    { "Diag",     "D",  0xFFFFFF, 0x2B50AA, VIEW_DIAG     },
    { "Settings", "*",  0xFFFFFF, 0x6E3E9E, VIEW_SETTINGS }
};

/* ── 5. Главный экран (Рабочий стол в точности по макету пользователя) ── */
static void draw_desktop_view(struct disp_state *s) {
    /* 1. Обои из ассетов (золотая роза) */
    draw_wallpaper(s);

    /* 2. Строка состояния */
    draw_status_bar(s);

    int w = (int)s->width;

    /* ── 3. Верхняя сетка 2 ряда × 4 колонки (iOS-стиль макета) ──── */
    int icon_sz = 228;
    int icon_r  = 54;
    int grid_y0 = 240;
    int row_gap = 60;
    int col_gap = (w - 128 - 4 * icon_sz) / 3; /* ~133 px */

    for (int i = 0; i < 8; i++) {
        int r = i / 4;
        int c = i % 4;
        int ix = 64 + c * (icon_sz + col_gap);
        int iy = grid_y0 + r * (icon_sz + row_gap);

        /* Белая скруглённая плитка как на макете пользователя */
        disp_rounded_rect(s, ix, iy, icon_sz, icon_sz, icon_r, g_grid_apps[i].tile_bg, 250);
        /* Тонкая объёмная обводка */
        disp_circle_outline(s, ix + icon_sz / 2, iy + icon_sz / 2, icon_sz / 2 - 2, 2, 0xDCD4C4, 180);

        /* Фирменный символ внутри */
        int sc_gl = 4;
        int gw = disp_text_width(g_grid_apps[i].glyph, sc_gl);
        int gh = DISP_FONT_HEIGHT * sc_gl;
        disp_draw_text(s, g_grid_apps[i].glyph,
                       ix + (icon_sz - gw) / 2,
                       iy + (icon_sz - gh) / 2,
                       sc_gl, g_grid_apps[i].glyph_col);

        /* Подпись под иконкой (читаемо на 1440x3040) */
        int sc_lbl = 2;
        int lw = disp_text_width(g_grid_apps[i].title, sc_lbl);
        disp_draw_text(s, g_grid_apps[i].title,
                       ix + (icon_sz - lw) / 2,
                       iy + icon_sz + 14,
                       sc_lbl, C_WHITE);
    }

    /* ── 4. Поисковая пилюля «Search 🔍» по центру (как на макете) ── */
    int search_w = 260;
    int search_h = 68;
    int search_x = (w - search_w) / 2;
    int search_y = 2370;

    disp_rounded_rect(s, search_x, search_y, search_w, search_h, 34, 0x483412, 160);
    disp_rect_outline(s, search_x, search_y, search_w, search_h, 2, 0x886B30);
    int stw = disp_text_width("Search", 2);
    disp_draw_text(s, "Search", search_x + (search_w - stw) / 2, search_y + 18, 2, C_WHITE);

    /* ── 5. Нижний Dock (полупрозрачное затемнённое стекло, 4 иконки) ── */
    int dock_w = w - 96;
    int dock_h = 340;
    int dock_x = 48;
    int dock_y = 2500;
    int dock_r = 75;

    /* Тень и матовый затемнённый фон дока */
    disp_fill_rect_alpha(s, dock_x + 10, dock_y + 10, dock_w, dock_h, 0x000000, 100);
    disp_rounded_rect(s, dock_x, dock_y, dock_w, dock_h, dock_r, 0x221606, 175);
    disp_rect_outline(s, dock_x, dock_y, dock_w, dock_h, 2, 0x5C4214);

    /* 4 иконки внутри дока */
    int dock_gap = (dock_w - 96 - 4 * icon_sz) / 3;
    int dock_iy  = dock_y + (dock_h - icon_sz) / 2;

    for (int i = 0; i < 4; i++) {
        int dix = dock_x + 48 + i * (icon_sz + dock_gap);

        /* Плитка в доке */
        disp_rounded_rect(s, dix, dock_iy, icon_sz, icon_sz, icon_r, g_dock_apps[i].tile_bg, 245);
        disp_circle_outline(s, dix + icon_sz / 2, dock_iy + icon_sz / 2, icon_sz / 2 - 2, 2, 0x8A7240, 160);

        int sc_gl = 4;
        int gw = disp_text_width(g_dock_apps[i].glyph, sc_gl);
        int gh = DISP_FONT_HEIGHT * sc_gl;
        disp_draw_text(s, g_dock_apps[i].glyph,
                       dix + (icon_sz - gw) / 2,
                       dock_iy + (icon_sz - gh) / 2,
                       sc_gl, g_dock_apps[i].glyph_col);
    }

    /* ── 6. Домашняя полоска (Home Indicator Bar) ─────────────────── */
    int pill_w = 420;
    int pill_h = 10;
    int pill_x = (w - pill_w) / 2;
    int pill_y = 2970;
    disp_rounded_rect(s, pill_x, pill_y, pill_w, pill_h, 5, C_WHITE, 190);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  ПОД-ЭКРАНЫ СИСТЕМЫ (ТЕРМИНАЛ, ДИАГНОСТИКА, ТАЧ-ИНСПЕКТОР И ДР.)
 * ═══════════════════════════════════════════════════════════════════════ */

/* ── Терминал ── */
static void terminal_execute(void);

static void draw_terminal_view(struct disp_state *s) {
    draw_wallpaper(s);
    draw_status_bar(s);
    draw_mobile_header(s, "Mobile Terminal", C_TERM_CMD);

    int scale  = ui_scale(s);
    int sb_h   = 140;
    int head_h = (s->height >= 2500) ? 120 : 64;
    int kb_h   = (s->height >= 2500) ? 1060 : (int)(s->height * 34 / 100);

    int win_x = 32;
    int win_y = sb_h + head_h + 20;
    int win_w = (int)s->width - 64;
    int win_h = (int)s->height - kb_h - win_y - 20;

    disp_rounded_rect(s, win_x, win_y, win_w, win_h, 20, 0x080604, 250);
    disp_rect_outline(s, win_x, win_y, win_w, win_h, 2, C_BORDER_HI);

    int ln_h      = DISP_FONT_HEIGHT * scale + 8;
    int max_lines = (win_h - 40) / ln_h;
    if (max_lines < 1) max_lines = 1;

    int first = g_term_history_count - max_lines + 2;
    if (first < 0) first = 0;
    int row = 0;

    disp_draw_text(s, "NexusOS Mobile Terminal v1.0 [type 'help']", win_x + 20, win_y + 16, scale, C_DIM);
    row++;

    for (int i = first; i < g_term_history_count && row < max_lines - 1; i++, row++) {
        disp_draw_text(s, g_term_history[i], win_x + 20, win_y + 16 + row * ln_h, scale, C_TERM_FG);
    }

    if (g_term_output[0] && row < max_lines) {
        disp_draw_text(s, g_term_output, win_x + 20, win_y + 16 + row * ln_h, scale, C_WHITE);
        row++;
    }

    const char *osk_buf = disp_osk_get_buffer();
    char line[160];
    int il = 0;
    const char *pr = "> ";
    while (*pr) line[il++] = *pr++;
    int bi = 0;
    while (osk_buf[bi] && il < 150) line[il++] = osk_buf[bi++];
    line[il++] = '_';
    line[il] = '\0';

    if (row < max_lines) {
        disp_draw_text(s, line, win_x + 20, win_y + 16 + row * ln_h, scale, C_TERM_CMD);
    }

    disp_osk_draw(s);
}

static void terminal_execute(void) {
    const char *cmd = disp_osk_get_buffer();

    if (g_term_history_count < 8) {
        scopy(g_term_history[g_term_history_count], cmd);
        g_term_history_count++;
    } else {
        for (int i = 0; i < 7; i++)
            scopy(g_term_history[i], g_term_history[i + 1]);
        scopy(g_term_history[7], cmd);
    }

    if (slen(cmd) == 0 || (cmd[0] == ' ' && slen(cmd) == 1)) {
        scopy(g_term_output, "");
    } else if (cmd[0]=='h' && cmd[1]=='e' && cmd[2]=='l' && cmd[3]=='p') {
        scopy(g_term_output, "Cmds: usb, touch, dmesg, fb, mem, reboot, clear, desk");
    } else if (cmd[0]=='u' && cmd[1]=='s' && cmd[2]=='b') {
        struct usb_gs101_status ust;
        usb_gs101_get_status(&ust);
        char out[96]; int oi = 0;
        const char *u1 = "DWC3: "; while (*u1) out[oi++] = *u1++;
        const char *u2 = ust.connected ? "CONNECTED (" : "DISCONNECTED ("; while (*u2) out[oi++] = *u2++;
        const char *u3 = usb_gs101_speed_str(); while (*u3) out[oi++] = *u3++;
        out[oi++] = ')';
        out[oi] = '\0';
        scopy(g_term_output, out);
    } else if (cmd[0]=='t' && cmd[1]=='o' && cmd[2]=='u' && cmd[3]=='c' && cmd[4]=='h') {
        struct touch_debug_info dbg;
        touch_raven_get_debug(&dbg);
        char out[96]; int oi = 0;
        const char *p1 = "SEC_TS polls="; while (*p1) out[oi++] = *p1++;
        char n1[12]; fmt_uint(dbg.poll_count, n1); int i1 = 0; while (n1[i1]) out[oi++] = n1[i1++];
        const char *p2 = " pkts="; while (*p2) out[oi++] = *p2++;
        char n2[12]; fmt_uint(dbg.packet_count, n2); int i2 = 0; while (n2[i2]) out[oi++] = n2[i2++];
        out[oi] = '\0';
        scopy(g_term_output, out);
    } else if (cmd[0]=='d' && cmd[1]=='m' && cmd[2]=='e' && cmd[3]=='s' && cmd[4]=='g') {
        g_view = VIEW_LOGS;
    } else if (cmd[0]=='f' && cmd[1]=='b') {
        scopy(g_term_output, "FB: 1440x3040 AMOLED, DPU IDMA, fmt=RGB32");
    } else if (cmd[0]=='m' && cmd[1]=='e' && cmd[2]=='m') {
        scopy(g_term_output, "DRAM: 12GB LPDDR5, MMU 2MB pages enabled");
    } else if (cmd[0]=='r' && cmd[1]=='e' && cmd[2]=='b' && cmd[3]=='o' && cmd[4]=='o' && cmd[5]=='t') {
        psci_system_reset();
    } else if (cmd[0]=='c' && cmd[1]=='l' && cmd[2]=='e' && cmd[3]=='a' && cmd[4]=='r') {
        g_term_history_count = 0;
        scopy(g_term_output, "");
    } else if (cmd[0]=='d' && cmd[1]=='e' && cmd[2]=='s' && cmd[3]=='k') {
        g_view = VIEW_DESKTOP;
    } else {
        scopy(g_term_output, "Unknown command. Type 'help'");
    }

    disp_osk_clear_buffer();
}

/* ── Аппаратная диагностика (включая регистры кнопок питания) ── */
static int draw_kv(struct disp_state *s, int x, int y, int row_h, int scale,
                   const char *key, const char *val, uint32_t vcol, uint32_t bgcol) {
    disp_fill_rect_alpha(s, x - 8, y - 4, (int)s->width - x * 2 + 16, row_h - 6, bgcol, 220);
    disp_draw_text(s, key, x, y, scale, C_DIM);
    int kw = disp_text_width("Power Button GPA10 ", scale);
    disp_draw_text(s, val, x + kw, y, scale, vcol);
    return y + row_h;
}

static void draw_diag_view(struct disp_state *s) {
    draw_wallpaper(s);
    draw_status_bar(s);
    draw_mobile_header(s, "Hardware Diagnostics", C_AMBER);

    int scale  = ui_scale(s);
    int sb_h   = 140;
    int head_h = (s->height >= 2500) ? 120 : 64;
    int cx     = 36;
    int cy     = sb_h + head_h + 20;
    int row_h  = DISP_FONT_HEIGHT * scale + 24;

    disp_draw_text(s, "[ Google Tensor GS101 SoC ]", cx, cy, scale, C_ACCENT);
    cy += row_h;
    cy = draw_kv(s, cx, cy, row_h, scale, "CPU Architecture", "ARMv8.2-A AArch64 EL1", C_WHITE, 0x1A1408);
    cy = draw_kv(s, cx, cy, row_h, scale, "CPU Cores       ", "2x X1 + 2x A76 + 4x A55", C_WHITE, 0x140E06);
    cy = draw_kv(s, cx, cy, row_h, scale, "Memory DRAM     ", "12 GB LPDDR5 3200MHz", C_WHITE, 0x1A1408);
    cy += 16;

    disp_draw_text(s, "[ Hardware Buttons / Keys ]", cx, cy, scale, C_ACCENT);
    cy += row_h;

    /* Живое отображение регистров кнопок питания и громкости */
    char pwr_buf[64];
    uint32_t a10 = btn_power_get_raw_a10();
    uint32_t pwr_pressed = !(a10 & (1U << 1));
    fmt_hex32(a10, pwr_buf);
    scopy(pwr_buf + 10, pwr_pressed ? " (PRESSED)" : " (IDLE)");
    cy = draw_kv(s, cx, cy, row_h, scale, "Power Key GPA10 ", pwr_buf, pwr_pressed ? C_ROSE : C_EMERALD, 0x1A1408);

    char vdn_buf[64];
    uint32_t a7 = btn_power_get_raw_a7();
    uint32_t vdn_pressed = !(a7 & (1U << 3));
    fmt_hex32(a7, vdn_buf);
    scopy(vdn_buf + 10, vdn_pressed ? " (PRESSED)" : " (IDLE)");
    cy = draw_kv(s, cx, cy, row_h, scale, "Vol-Down GPA7   ", vdn_buf, vdn_pressed ? C_AMBER : C_EMERALD, 0x140E06);

    char vup_buf[64];
    uint32_t a8 = btn_power_get_raw_a8();
    uint32_t vup_pressed = !(a8 & (1U << 1));
    fmt_hex32(a8, vup_buf);
    scopy(vup_buf + 10, vup_pressed ? " (PRESSED)" : " (IDLE)");
    cy = draw_kv(s, cx, cy, row_h, scale, "Vol-Up GPA8     ", vup_buf, vup_pressed ? C_AMBER : C_EMERALD, 0x1A1408);
    cy += 16;

    disp_draw_text(s, "[ Display & Touch Subsystem ]", cx, cy, scale, C_ACCENT);
    cy += row_h;
    cy = draw_kv(s, cx, cy, row_h, scale, "Display Panel   ", "1440x3040 AMOLED 120Hz", C_WHITE, 0x1A1408);
    cy = draw_kv(s, cx, cy, row_h, scale, "Touchscreen SPI ", "Samsung SEC TS (SPI_11)", C_EMERALD, 0x140E06);
}

/* ── Тач-инспектор ── */
static void draw_touch_debug_view(struct disp_state *s) {
    draw_wallpaper(s);
    draw_status_bar(s);
    draw_mobile_header(s, "Touch Inspector (Samsung SEC)", C_PINK);

    int scale  = ui_scale(s);
    int sb_h   = 140;
    int head_h = (s->height >= 2500) ? 120 : 64;

    struct touch_debug_info dbg;
    touch_raven_get_debug(&dbg);

    int cx    = 32;
    int cy    = sb_h + head_h + 20;
    int win_w = (int)s->width - 64;
    int row_h = DISP_FONT_HEIGHT * scale + 24;

    uint32_t status_col = dbg.hw_active ? C_EMERALD : C_AMBER;
    const char *st_text = dbg.hw_active ? "SEC_TS ONLINE - SPI11 Events Active"
                                        : "SEC_TS Awaiting Touch Input...";
    disp_rounded_rect(s, cx, cy, win_w, row_h + 8, 14, 0x1E1408, 240);
    disp_circle(s, cx + 24, cy + (row_h + 8) / 2, 10, status_col, 255);
    disp_draw_text(s, st_text, cx + 46, cy + 12, scale, status_col);
    cy += row_h + 20;

    char buf[32];
    { int bi = 0; char n[12]; fmt_uint(dbg.poll_count, n);
      int ni = 0; while (n[ni]) buf[bi++] = n[ni++]; buf[bi] = '\0'; }
    cy = draw_kv(s, cx, cy, row_h, scale, "Poll Count    ", buf, C_WHITE, 0x1A1408);

    { int bi = 0; char n[12]; fmt_uint(dbg.packet_count, n);
      int ni = 0; while (n[ni]) buf[bi++] = n[ni++]; buf[bi] = '\0'; }
    cy = draw_kv(s, cx, cy, row_h, scale, "Packet Count  ", buf, C_ACCENT, 0x140E06);

    fmt_hex32(dbg.spi_status, buf);
    cy = draw_kv(s, cx, cy, row_h, scale, "SPI Status Reg", buf, C_EMERALD, 0x1A1408);

    /* 8 сырых байт пакета */
    disp_draw_text(s, "Raw Payload [8 Bytes]:", cx, cy + 10, scale, C_DIM);
    cy += DISP_FONT_HEIGHT * scale + 20;

    int cell_w = (win_w - 42) / 8;
    int cell_h = DISP_FONT_HEIGHT * scale + 12;
    for (int i = 0; i < 8; i++) {
        int bx = cx + i * (cell_w + 6);
        disp_rounded_rect(s, bx, cy, cell_w, cell_h, 8, 0x2A1C0A, 240);
        char hb[6]; fmt_hex8(dbg.last_rx_payload[i], hb);
        disp_draw_text(s, hb, bx + 6, cy + 6, scale, C_GOLD);
    }
}

/* ── Холст для рисования и калибровки ── */
static void draw_touch_test_view(struct disp_state *s) {
    draw_wallpaper(s);
    draw_status_bar(s);
    draw_mobile_header(s, "Touch Canvas & Test", C_EMERALD);

    int scale  = ui_scale(s);
    int sb_h   = 140;
    int head_h = (s->height >= 2500) ? 120 : 64;
    int w      = (int)s->width;

    /* Кнопка [Clear Canvas] */
    int clr_w = disp_text_width("Clear Canvas", scale) + 32;
    int clr_x = w - clr_w - 40;
    int clr_y = sb_h + head_h + 16;
    disp_rounded_rect(s, clr_x, clr_y, clr_w, 48, 12, 0x361622, 240);
    disp_rect_outline(s, clr_x, clr_y, clr_w, 48, 2, C_ROSE);
    disp_draw_text(s, "Clear Canvas", clr_x + 16, clr_y + 12, scale, C_WHITE);

    /* Отрисовка линий и точек */
    static const uint32_t trail_colors[6] = {
        0x38BDF8, 0x34D399, 0xFF6B9D, 0xFBBF24, 0xE8B84B, 0xFFFFFF
    };
    for (int i = 0; i < g_canvas_count; i++) {
        int cx = (int)g_canvas_x[i];
        int cy = (int)g_canvas_y[i];
        uint32_t col = trail_colors[g_canvas_color[i] % 6];
        disp_circle(s, cx, cy, 22, col, 220);
        disp_circle(s, cx, cy, 10, 0xFFFFFF, 255);
    }
}

/* ── Логи ядра (Dmesg / Ramoops) ── */
static void draw_logs_view(struct disp_state *s) {
    draw_wallpaper(s);
    draw_status_bar(s);
    draw_mobile_header(s, "Kernel Logs (Ramoops)", C_AMBER);

    int scale    = ui_scale(s);
    int sb_h     = 140;
    int head_h   = (s->height >= 2500) ? 120 : 64;
    int w        = (int)s->width;

    int win_x = 32;
    int win_y = sb_h + head_h + 20;
    int win_w = w - 64;
    int win_h = (int)s->height - win_y - 120;

    disp_rounded_rect(s, win_x, win_y, win_w, win_h, 20, 0x060503, 250);
    disp_rect_outline(s, win_x, win_y, win_w, win_h, 2, C_BORDER_HI);

    uint32_t log_size = ramoops_pos;
    if (log_size > RAMOOPS_DATA_MAX) log_size = RAMOOPS_DATA_MAX;

    int ln_h      = DISP_FONT_HEIGHT * scale + 6;
    int max_lines = (win_h - 32) / ln_h;
    if (max_lines < 1) max_lines = 1;

    int total_lines = 1;
    for (uint32_t i = 0; i < log_size; i++) {
        if (ramoops_buf->data[i] == '\n') total_lines++;
    }

    int start_line = total_lines - max_lines - g_log_scroll_offset;
    if (start_line < 0) start_line = 0;

    int current_line = 0;
    int drawn_lines  = 0;
    char line_buf[128];
    int  li = 0;

    for (uint32_t i = 0; i < log_size && drawn_lines < max_lines; i++) {
        char ch = (char)ramoops_buf->data[i];
        if (ch == '\n') {
            if (current_line >= start_line) {
                line_buf[li] = '\0';
                uint32_t col = C_TERM_FG;
                if (line_buf[0] == '[') col = C_GOLD;
                disp_draw_text(s, line_buf, win_x + 16, win_y + 16 + drawn_lines * ln_h, scale, col);
                drawn_lines++;
            }
            current_line++;
            li = 0;
        } else {
            if (li < 120 && ch >= 32 && ch <= 126) {
                line_buf[li++] = ch;
            }
        }
    }
}

/* ── Настройки ── */
static const char *g_settings_items[5] = {
    "Version:   NexusOS Mobile 1.0 (aarch64)",
    "Display:   1440x3040 AMOLED 120Hz",
    "Touch:     Samsung SEC (SPI_11 @ 1MHz)",
    "Memory:    12 GB LPDDR5",
    "About:     Google Pixel 6 Pro (raven)"
};

static void draw_settings_view(struct disp_state *s) {
    draw_wallpaper(s);
    draw_status_bar(s);
    draw_mobile_header(s, "Settings", C_GOLD);

    int scale  = ui_scale(s);
    int sb_h   = 140;
    int head_h = (s->height >= 2500) ? 120 : 64;
    int cy     = sb_h + head_h + 24;
    int row_h  = DISP_FONT_HEIGHT * scale + 36;
    int win_w  = (int)s->width - 48;

    for (int i = 0; i < 5; i++) {
        int ry = cy + i * row_h;
        int is_sel = (i == g_settings_sel);
        disp_rounded_rect(s, 24, ry, win_w, row_h - 10, 14, is_sel ? 0x362810 : 0x1A1408, 240);
        if (is_sel) disp_fill_rect(s, 24, ry, 6, row_h - 10, C_GOLD);
        disp_draw_text(s, g_settings_items[i], 44, ry + (row_h - 10 - DISP_FONT_HEIGHT * scale) / 2, scale,
                       is_sel ? C_WHITE : C_DIM);
    }
}

/* ── Файлы (RAM-VFS) ── */
static void draw_files_view(struct disp_state *s) {
    draw_wallpaper(s);
    draw_status_bar(s);
    draw_mobile_header(s, "Filesystem Explorer", C_EMERALD);

    int scale  = ui_scale(s);
    int sb_h   = 140;
    int head_h = (s->height >= 2500) ? 120 : 64;
    int cy     = sb_h + head_h + 30;

    disp_draw_text_center(s, "/  (RAM-VFS Root Directory)", cy, scale, C_WHITE);
    cy += DISP_FONT_HEIGHT * scale + 30;

    static const char *dirs[6] = { "bin/", "dev/", "etc/", "lib/", "proc/", "sys/" };
    int row_h = DISP_FONT_HEIGHT * scale + 32;
    int col_w = ((int)s->width - 48) / 3;

    for (int i = 0; i < 6; i++) {
        int col = i % 3;
        int row = i / 3;
        int ix  = 24 + col * col_w + 20;
        int iy  = cy + row * row_h;
        disp_circle(s, ix, iy + DISP_FONT_HEIGHT * scale / 2, 12, C_EMERALD, 220);
        disp_draw_text(s, dirs[i], ix + 24, iy, scale, C_WHITE);
    }
}

/* ═══════════════════════════════════════════════════════════════════════
 *  ПУБЛИЧНЫЙ API: ОТРИСОВКА И ОБРАБОТКА КАСАНИЙ
 * ═══════════════════════════════════════════════════════════════════════ */
void disp_ui_draw(struct disp_state *s, const struct nexus_sysinfo *info) {
    if (!s || !s->fb) return;
    (void)info;

    switch (g_view) {
    case VIEW_TERMINAL:   draw_terminal_view(s);   break;
    case VIEW_SETTINGS:   draw_settings_view(s);   break;
    case VIEW_FILES:      draw_files_view(s);      break;
    case VIEW_DIAG:       draw_diag_view(s);       break;
    case VIEW_TOUCH_DBG:  draw_touch_debug_view(s);break;
    case VIEW_TOUCH_TEST: draw_touch_test_view(s); break;
    case VIEW_LOGS:       draw_logs_view(s);       break;
    default:              draw_desktop_view(s);    break;
    }
}

void disp_ui_update_heartbeat(struct disp_state *s, uint32_t seconds) {
    if (!s || !s->fb) return;
    g_uptime_sec = seconds;
    disp_ui_draw(s, 0);
}

/* ── Обработка тач-событий ───────────────────────────────────────────── */
int disp_ui_handle_touch(struct disp_state *s, int raw_tx, int raw_ty, int pressed) {
    if (!s || !s->fb || !pressed) return 0;

    int tx = raw_tx;
    int ty = raw_ty;
    if (TOUCH_PANEL_MAX_X > 0 && s->width > 0 && s->width != TOUCH_PANEL_MAX_X) {
        tx = (int)((uint64_t)raw_tx * s->width / TOUCH_PANEL_MAX_X);
    }
    if (TOUCH_PANEL_MAX_Y > 0 && s->height > 0 && s->height != TOUCH_PANEL_MAX_Y) {
        ty = (int)((uint64_t)raw_ty * s->height / TOUCH_PANEL_MAX_Y);
    }

    g_last_tap_x = tx;
    g_last_tap_y = ty;
    g_touch_detected = 1;
    touch_hist_push((uint16_t)tx, (uint16_t)ty, (uint8_t)pressed);

    int scale  = ui_scale(s);
    int sb_h   = 140;
    int head_h = (s->height >= 2500) ? 120 : 64;
    int w      = (int)s->width;

    /* 1. Кнопка «< Home» в любом под-экране */
    if (g_view != VIEW_DESKTOP) {
        int btn_w = disp_text_width("< Home", scale) + 40;
        int btn_h = head_h - 24;
        int btn_y = sb_h + 12;
        if (tx >= 32 && tx <= 32 + btn_w && ty >= btn_y && ty <= btn_y + btn_h) {
            g_view = VIEW_DESKTOP;
            disp_osk_set_visible(0);
            disp_ui_draw(s, 0);
            return 1;
        }
    }

    /* 2. Под-экран: Терминал (клавиатура OSK) */
    if (g_view == VIEW_TERMINAL) {
        int kb_h = (s->height >= 2500) ? 1060 : (int)(s->height * 34 / 100);
        int kb_y = (int)s->height - kb_h;
        if (ty >= kb_y) {
            int r = disp_osk_handle_touch(s, tx, ty, pressed);
            if (disp_osk_was_entered()) {
                disp_osk_clear_enter_flag();
                terminal_execute();
            }
            disp_ui_draw(s, 0);
            return r;
        }
    }

    /* 3. Под-экран: Touch Test (холст и очистка) */
    if (g_view == VIEW_TOUCH_TEST) {
        int clr_w = disp_text_width("Clear Canvas", scale) + 32;
        int clr_x = w - clr_w - 40;
        int clr_y = sb_h + head_h + 16;
        if (tx >= clr_x && tx <= clr_x + clr_w && ty >= clr_y && ty <= clr_y + 48) {
            g_canvas_count = 0;
            g_canvas_head  = 0;
            disp_ui_draw(s, 0);
            return 1;
        }

        if (ty > sb_h + head_h) {
            if (g_canvas_count < CANVAS_MAX_POINTS) {
                g_canvas_x[g_canvas_count] = (int16_t)tx;
                g_canvas_y[g_canvas_count] = (int16_t)ty;
                g_canvas_color[g_canvas_count] = (uint8_t)(g_canvas_count % 6);
                g_canvas_count++;
            } else {
                g_canvas_x[g_canvas_head] = (int16_t)tx;
                g_canvas_y[g_canvas_head] = (int16_t)ty;
                g_canvas_head = (g_canvas_head + 1) % CANVAS_MAX_POINTS;
            }
            disp_ui_draw(s, 0);
            return 1;
        }
    }

    /* ── 4. Рабочий стол (Home Screen) ────────────────────────────── */
    if (g_view == VIEW_DESKTOP) {
        /* А. Нажатие на верхнюю сетку 2×4 */
        int icon_sz = 228;
        int grid_y0 = 240;
        int row_gap = 60;
        int col_gap = (w - 128 - 4 * icon_sz) / 3;

        for (int i = 0; i < 8; i++) {
            int r = i / 4;
            int c = i % 4;
            int ix = 64 + c * (icon_sz + col_gap);
            int iy = grid_y0 + r * (icon_sz + row_gap);

            if (tx >= ix && tx < ix + icon_sz && ty >= iy && ty < iy + icon_sz) {
                int target = g_grid_apps[i].target_view;
                if (target == -1) {
                    /* Reboot */
                    psci_system_reset();
                } else {
                    g_view = target;
                    if (g_view == VIEW_TERMINAL) disp_osk_set_visible(1);
                    else disp_osk_set_visible(0);
                    disp_ui_draw(s, 0);
                    return 1;
                }
            }
        }

        /* Б. Нажатие на поисковую капсулу «Search» */
        int search_w = 260;
        int search_h = 68;
        int search_x = (w - search_w) / 2;
        int search_y = 2370;
        if (tx >= search_x && tx <= search_x + search_w &&
            ty >= search_y && ty <= search_y + search_h) {
            g_view = VIEW_TERMINAL;
            disp_osk_set_visible(1);
            disp_ui_draw(s, 0);
            return 1;
        }

        /* В. Нажатие на 4 иконки в Dock */
        int dock_w = w - 96;
        int dock_h = 340;
        int dock_x = 48;
        int dock_y = 2500;
        int dock_gap = (dock_w - 96 - 4 * icon_sz) / 3;
        int dock_iy  = dock_y + (dock_h - icon_sz) / 2;

        for (int i = 0; i < 4; i++) {
            int dix = dock_x + 48 + i * (icon_sz + dock_gap);
            if (tx >= dix && tx < dix + icon_sz &&
                ty >= dock_iy && ty < dock_iy + icon_sz) {
                int target = g_dock_apps[i].target_view;
                g_view = target;
                if (g_view == VIEW_TERMINAL) disp_osk_set_visible(1);
                else disp_osk_set_visible(0);
                disp_ui_draw(s, 0);
                return 1;
            }
        }
    }

    disp_ui_draw(s, 0);
    return 1;
}
