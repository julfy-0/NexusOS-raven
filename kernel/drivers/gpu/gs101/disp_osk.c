#include "a64.h"
#include "gs101_disp.h"
#include "disp_draw.h"
#include "disp_osk.h"

/* Цвета экранной клавиатуры */
#define COLOR_OSK_BG        0x0B0F19
#define COLOR_OSK_BORDER    0x1E293B
#define COLOR_KEY_BG        0x162032
#define COLOR_KEY_SPECIAL   0x1E2B42
#define COLOR_KEY_ACTIVE    0x38BDF8
#define COLOR_KEY_TEXT      0xF8FAFC
#define COLOR_KEY_BORDER    0x2E3E58
#define COLOR_INPUT_BG      0x05080F

static struct osk_state g_osk;
static int g_osk_enter_flag = 0;

/* Символы клавиш: буквы строчные, прописные и спецсимволы */
static const char *row0_chars = "1234567890";
static const char *row0_syms  = "!@#$%^&*()";

static const char *row1_chars = "qwertyuiop";
static const char *row1_caps  = "QWERTYUIOP";
static const char *row1_syms  = "~`-_=+[]{}";

static const char *row2_chars = "asdfghjkl";
static const char *row2_caps  = "ASDFGHJKL";
static const char *row2_syms  = ":;\"'\\|<>";

static const char *row3_chars = "zxcvbnm";
static const char *row3_caps  = "ZXCVBNM";
static const char *row3_syms  = ",.?/!@#";

void disp_osk_init(void) {
    g_osk.visible = 1;
    g_osk.shift = 0;
    g_osk.sym_mode = 0;
    g_osk.pressed_key_idx = -1;
    g_osk.buffer_len = 0;
    g_osk.buffer[0] = '\0';
    g_osk.last_touch_x = -1;
    g_osk.last_touch_y = -1;
}

void disp_osk_set_visible(int visible) { g_osk.visible = visible; }
int  disp_osk_is_visible(void) { return g_osk.visible; }
void disp_osk_toggle(void) { g_osk.visible = !g_osk.visible; }
const char *disp_osk_get_buffer(void) { return g_osk.buffer; }

void disp_osk_clear_buffer(void) {
    g_osk.buffer_len = 0;
    g_osk.buffer[0] = '\0';
}

int disp_osk_was_entered(void) { return g_osk_enter_flag; }
void disp_osk_clear_enter_flag(void) { g_osk_enter_flag = 0; }

static void osk_append_char(char c) {
    if (g_osk.buffer_len < OSK_BUFFER_MAX - 1) {
        g_osk.buffer[g_osk.buffer_len++] = c;
        g_osk.buffer[g_osk.buffer_len] = '\0';
    }
}

static void osk_backspace(void) {
    if (g_osk.buffer_len > 0) {
        g_osk.buffer[--g_osk.buffer_len] = '\0';
    }
}

void disp_osk_draw(struct disp_state *s) {
    if (!s || !s->fb || !g_osk.visible) return;

    /* Для разрешения 1440x3120 масштабируем клавиатуру пропорционально */
    int scale = (s->width >= 1200) ? 3 : ((s->width >= 800) ? 2 : 1);
    int kb_h = (s->height >= 2500) ? 1060 : (int)(s->height * 34 / 100);
    int kb_y = (int)s->height - kb_h;
    int kb_w = (int)s->width;

    /* 1. Фон панели клавиатуры */
    disp_fill_rect(s, 0, kb_y, kb_w, kb_h, COLOR_OSK_BG);
    disp_hline(s, 0, kb_y, kb_w, COLOR_OSK_BORDER);
    disp_hline(s, 0, kb_y + 1, kb_w, 0x334155);

    /* 2. Поле ввода командной строки */
    int inp_margin = (int)(s->width * 2 / 100);
    int inp_w = kb_w - inp_margin * 2;
    int inp_h = (s->height >= 2500) ? 90 : 64;
    int inp_y = kb_y + 16;

    disp_rounded_rect(s, inp_margin, inp_y, inp_w, inp_h, 14, COLOR_INPUT_BG, 255);
    disp_rect_outline(s, inp_margin, inp_y, inp_w, inp_h, 2, 0x223049);

    /* Текст командной строки */
    char prompt_display[160];
    int p_idx = 0;
    const char *pr = "> shell: ";
    while (*pr) prompt_display[p_idx++] = *pr++;
    int b_idx = 0;
    while (g_osk.buffer[b_idx] && p_idx < 150) prompt_display[p_idx++] = g_osk.buffer[b_idx++];
    prompt_display[p_idx++] = '_';
    prompt_display[p_idx] = '\0';

    int txt_y = inp_y + (inp_h - DISP_FONT_HEIGHT * scale) / 2;
    disp_draw_text(s, prompt_display, inp_margin + 20, txt_y, scale, 0x38BDF8);

    /* 3. Геометрия клавиш */
    int key_margin_x = inp_margin;
    int avail_w = kb_w - key_margin_x * 2;
    int key_gap = (s->height >= 2500) ? 14 : 8;
    int row_gap = (s->height >= 2500) ? 16 : 10;
    int key_h = (s->height >= 2500) ? 145 : 88;
    int r_radius = (s->height >= 2500) ? 16 : 10;

    int start_y = inp_y + inp_h + 20;

    /* Ряд 0: Числа (10 клавиш) */
    int k_w10 = (avail_w - key_gap * 9) / 10;
    for (int i = 0; i < 10; i++) {
        int kx = key_margin_x + i * (k_w10 + key_gap);
        int ky = start_y;
        char c = g_osk.sym_mode ? row0_syms[i] : row0_chars[i];

        disp_rounded_rect(s, kx, ky, k_w10, key_h, r_radius, COLOR_KEY_BG, 255);
        disp_rect_outline(s, kx, ky, k_w10, key_h, 2, COLOR_KEY_BORDER);
        char str[2] = { c, '\0' };
        disp_draw_text(s, str, kx + (k_w10 - DISP_FONT_WIDTH * scale) / 2, ky + (key_h - DISP_FONT_HEIGHT * scale) / 2, scale, COLOR_KEY_TEXT);
    }

    /* Ряд 1: QWERTY (10 клавиш) */
    int r1_y = start_y + key_h + row_gap;
    for (int i = 0; i < 10; i++) {
        int kx = key_margin_x + i * (k_w10 + key_gap);
        int ky = r1_y;
        char c;
        if (g_osk.sym_mode) c = row1_syms[i];
        else c = g_osk.shift ? row1_caps[i] : row1_chars[i];

        disp_rounded_rect(s, kx, ky, k_w10, key_h, r_radius, COLOR_KEY_BG, 255);
        disp_rect_outline(s, kx, ky, k_w10, key_h, 2, COLOR_KEY_BORDER);
        char str[2] = { c, '\0' };
        disp_draw_text(s, str, kx + (k_w10 - DISP_FONT_WIDTH * scale) / 2, ky + (key_h - DISP_FONT_HEIGHT * scale) / 2, scale, COLOR_KEY_TEXT);
    }

    /* Ряд 2: ASDFGHJKL (9 клавиш с центрированием) */
    int r2_y = r1_y + key_h + row_gap;
    int r2_offset = (k_w10 + key_gap) / 2;
    for (int i = 0; i < 9; i++) {
        int kx = key_margin_x + r2_offset + i * (k_w10 + key_gap);
        int ky = r2_y;
        char c;
        if (g_osk.sym_mode) c = row2_syms[i];
        else c = g_osk.shift ? row2_caps[i] : row2_chars[i];

        disp_rounded_rect(s, kx, ky, k_w10, key_h, r_radius, COLOR_KEY_BG, 255);
        disp_rect_outline(s, kx, ky, k_w10, key_h, 2, COLOR_KEY_BORDER);
        char str[2] = { c, '\0' };
        disp_draw_text(s, str, kx + (k_w10 - DISP_FONT_WIDTH * scale) / 2, ky + (key_h - DISP_FONT_HEIGHT * scale) / 2, scale, COLOR_KEY_TEXT);
    }

    /* Ряд 3: SHIFT + ZXCVBNM + BACKSPACE */
    int r3_y = r2_y + key_h + row_gap;
    int shift_w = (int)((k_w10 * 16) / 10);
    int bksp_w  = (int)((k_w10 * 16) / 10);
    int mid_w   = avail_w - shift_w - bksp_w - key_gap * 8;
    int k_w7    = mid_w / 7;

    /* Клавиша Shift */
    uint32_t shift_bg = g_osk.shift ? COLOR_KEY_ACTIVE : COLOR_KEY_SPECIAL;
    uint32_t shift_fg = g_osk.shift ? 0x0B0F19 : COLOR_KEY_TEXT;
    disp_rounded_rect(s, key_margin_x, r3_y, shift_w, key_h, r_radius, shift_bg, 255);
    disp_rect_outline(s, key_margin_x, r3_y, shift_w, key_h, 2, COLOR_KEY_BORDER);
    disp_draw_text(s, "SHF", key_margin_x + (shift_w - 3 * DISP_FONT_WIDTH * scale) / 2, r3_y + (key_h - DISP_FONT_HEIGHT * scale) / 2, scale, shift_fg);

    /* 7 буквенных клавиш */
    for (int i = 0; i < 7; i++) {
        int kx = key_margin_x + shift_w + key_gap + i * (k_w7 + key_gap);
        char c;
        if (g_osk.sym_mode) c = row3_syms[i];
        else c = g_osk.shift ? row3_caps[i] : row3_chars[i];

        disp_rounded_rect(s, kx, r3_y, k_w7, key_h, r_radius, COLOR_KEY_BG, 255);
        disp_rect_outline(s, kx, r3_y, k_w7, key_h, 2, COLOR_KEY_BORDER);
        char str[2] = { c, '\0' };
        disp_draw_text(s, str, kx + (k_w7 - DISP_FONT_WIDTH * scale) / 2, r3_y + (key_h - DISP_FONT_HEIGHT * scale) / 2, scale, COLOR_KEY_TEXT);
    }

    /* Клавиша Backspace */
    int bksp_x = avail_w + key_margin_x - bksp_w;
    disp_rounded_rect(s, bksp_x, r3_y, bksp_w, key_h, r_radius, COLOR_KEY_SPECIAL, 255);
    disp_rect_outline(s, bksp_x, r3_y, bksp_w, key_h, 2, COLOR_KEY_BORDER);
    disp_draw_text(s, "DEL", bksp_x + (bksp_w - 3 * DISP_FONT_WIDTH * scale) / 2, r3_y + (key_h - DISP_FONT_HEIGHT * scale) / 2, scale, 0xFCA5A5);

    /* Ряд 4: ?123, /, SPACE, ., ENTER */
    int r4_y = r3_y + key_h + row_gap;
    int sym_btn_w = (int)(k_w10 * 18 / 10);
    int slash_btn_w = (int)(k_w10 * 11 / 10);
    int dot_btn_w = (int)(k_w10 * 11 / 10);
    int enter_btn_w = (int)(k_w10 * 22 / 10);
    int space_w = avail_w - sym_btn_w - slash_btn_w - dot_btn_w - enter_btn_w - key_gap * 4;

    int cur_x = key_margin_x;

    /* ?123 / ABC */
    const char *sym_lbl = g_osk.sym_mode ? "ABC" : "?123";
    disp_rounded_rect(s, cur_x, r4_y, sym_btn_w, key_h, r_radius, COLOR_KEY_SPECIAL, 255);
    disp_rect_outline(s, cur_x, r4_y, sym_btn_w, key_h, 2, COLOR_KEY_BORDER);
    disp_draw_text(s, sym_lbl, cur_x + (sym_btn_w - 4 * DISP_FONT_WIDTH * scale) / 2, r4_y + (key_h - DISP_FONT_HEIGHT * scale) / 2, scale, COLOR_KEY_TEXT);
    cur_x += sym_btn_w + key_gap;

    /* '/' */
    disp_rounded_rect(s, cur_x, r4_y, slash_btn_w, key_h, r_radius, COLOR_KEY_BG, 255);
    disp_rect_outline(s, cur_x, r4_y, slash_btn_w, key_h, 2, COLOR_KEY_BORDER);
    disp_draw_text(s, "/", cur_x + (slash_btn_w - DISP_FONT_WIDTH * scale) / 2, r4_y + (key_h - DISP_FONT_HEIGHT * scale) / 2, scale, COLOR_KEY_TEXT);
    cur_x += slash_btn_w + key_gap;

    /* SPACE */
    disp_rounded_rect(s, cur_x, r4_y, space_w, key_h, r_radius, COLOR_KEY_BG, 255);
    disp_rect_outline(s, cur_x, r4_y, space_w, key_h, 2, COLOR_KEY_BORDER);
    disp_draw_text(s, "SPACE", cur_x + (space_w - 5 * DISP_FONT_WIDTH * scale) / 2, r4_y + (key_h - DISP_FONT_HEIGHT * scale) / 2, scale, 0x94A3B8);
    cur_x += space_w + key_gap;

    /* '.' */
    disp_rounded_rect(s, cur_x, r4_y, dot_btn_w, key_h, r_radius, COLOR_KEY_BG, 255);
    disp_rect_outline(s, cur_x, r4_y, dot_btn_w, key_h, 2, COLOR_KEY_BORDER);
    disp_draw_text(s, ".", cur_x + (dot_btn_w - DISP_FONT_WIDTH * scale) / 2, r4_y + (key_h - DISP_FONT_HEIGHT * scale) / 2, scale, COLOR_KEY_TEXT);
    cur_x += dot_btn_w + key_gap;

    /* ENTER */
    disp_rounded_rect(s, cur_x, r4_y, enter_btn_w, key_h, r_radius, 0x065F46, 255);
    disp_rect_outline(s, cur_x, r4_y, enter_btn_w, key_h, 2, 0x34D399);
    disp_draw_text(s, "RUN", cur_x + (enter_btn_w - 3 * DISP_FONT_WIDTH * scale) / 2, r4_y + (key_h - DISP_FONT_HEIGHT * scale) / 2, scale, 0xFFFFFF);
}

int disp_osk_handle_touch(struct disp_state *s, int tx, int ty, int pressed) {
    if (!s || !s->fb || !g_osk.visible || !pressed) return 0;

    int kb_h = (s->height >= 2500) ? 1060 : (int)(s->height * 34 / 100);
    int kb_y = (int)s->height - kb_h;
    int kb_w = (int)s->width;

    if (ty < kb_y) return 0;

    int inp_margin = (int)(s->width * 2 / 100);
    int key_margin_x = inp_margin;
    int avail_w = kb_w - key_margin_x * 2;
    int key_gap = (s->height >= 2500) ? 14 : 8;
    int row_gap = (s->height >= 2500) ? 16 : 10;
    int key_h = (s->height >= 2500) ? 145 : 88;
    int inp_h = (s->height >= 2500) ? 90 : 64;
    int inp_y = kb_y + 16;
    int start_y = inp_y + inp_h + 20;

    /* Ряд 0 */
    int k_w10 = (avail_w - key_gap * 9) / 10;
    if (ty >= start_y && ty < start_y + key_h) {
        for (int i = 0; i < 10; i++) {
            int kx = key_margin_x + i * (k_w10 + key_gap);
            if (tx >= kx && tx < kx + k_w10) {
                osk_append_char(g_osk.sym_mode ? row0_syms[i] : row0_chars[i]);
                return 1;
            }
        }
    }

    /* Ряд 1 */
    int r1_y = start_y + key_h + row_gap;
    if (ty >= r1_y && ty < r1_y + key_h) {
        for (int i = 0; i < 10; i++) {
            int kx = key_margin_x + i * (k_w10 + key_gap);
            if (tx >= kx && tx < kx + k_w10) {
                if (g_osk.sym_mode) osk_append_char(row1_syms[i]);
                else osk_append_char(g_osk.shift ? row1_caps[i] : row1_chars[i]);
                return 1;
            }
        }
    }

    /* Ряд 2 */
    int r2_y = r1_y + key_h + row_gap;
    int r2_offset = (k_w10 + key_gap) / 2;
    if (ty >= r2_y && ty < r2_y + key_h) {
        for (int i = 0; i < 9; i++) {
            int kx = key_margin_x + r2_offset + i * (k_w10 + key_gap);
            if (tx >= kx && tx < kx + k_w10) {
                if (g_osk.sym_mode) osk_append_char(row2_syms[i]);
                else osk_append_char(g_osk.shift ? row2_caps[i] : row2_chars[i]);
                return 1;
            }
        }
    }

    /* Ряд 3 */
    int r3_y = r2_y + key_h + row_gap;
    int shift_w = (int)((k_w10 * 16) / 10);
    int bksp_w  = (int)((k_w10 * 16) / 10);
    int mid_w   = avail_w - shift_w - bksp_w - key_gap * 8;
    int k_w7    = mid_w / 7;

    if (ty >= r3_y && ty < r3_y + key_h) {
        if (tx >= key_margin_x && tx < key_margin_x + shift_w) {
            g_osk.shift = !g_osk.shift;
            return 1;
        }
        for (int i = 0; i < 7; i++) {
            int kx = key_margin_x + shift_w + key_gap + i * (k_w7 + key_gap);
            if (tx >= kx && tx < kx + k_w7) {
                if (g_osk.sym_mode) osk_append_char(row3_syms[i]);
                else osk_append_char(g_osk.shift ? row3_caps[i] : row3_chars[i]);
                return 1;
            }
        }
        int bksp_x = avail_w + key_margin_x - bksp_w;
        if (tx >= bksp_x && tx < bksp_x + bksp_w) {
            osk_backspace();
            return 1;
        }
    }

    /* Ряд 4 */
    int r4_y = r3_y + key_h + row_gap;
    int sym_btn_w = (int)(k_w10 * 18 / 10);
    int slash_btn_w = (int)(k_w10 * 11 / 10);
    int dot_btn_w = (int)(k_w10 * 11 / 10);
    int enter_btn_w = (int)(k_w10 * 22 / 10);
    int space_w = avail_w - sym_btn_w - slash_btn_w - dot_btn_w - enter_btn_w - key_gap * 4;

    if (ty >= r4_y && ty < r4_y + key_h) {
        int cur_x = key_margin_x;
        if (tx >= cur_x && tx < cur_x + sym_btn_w) {
            g_osk.sym_mode = !g_osk.sym_mode;
            return 1;
        }
        cur_x += sym_btn_w + key_gap;

        if (tx >= cur_x && tx < cur_x + slash_btn_w) {
            osk_append_char('/');
            return 1;
        }
        cur_x += slash_btn_w + key_gap;

        if (tx >= cur_x && tx < cur_x + space_w) {
            osk_append_char(' ');
            return 1;
        }
        cur_x += space_w + key_gap;

        if (tx >= cur_x && tx < cur_x + dot_btn_w) {
            osk_append_char('.');
            return 1;
        }
        cur_x += dot_btn_w + key_gap;

        if (tx >= cur_x && tx < cur_x + enter_btn_w) {
            g_osk_enter_flag = 1;
            return 1;
        }
    }

    return 1;
}
