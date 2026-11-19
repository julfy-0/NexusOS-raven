#include "a64.h"
#include "gs101_disp.h"
#include "disp_draw.h"
#include "font8x16.h"

void disp_px(struct disp_state *s, int x, int y, uint8_t r, uint8_t g, uint8_t b) {
    if (!s->fb || x < 0 || y < 0 || (uint32_t)x >= s->width || (uint32_t)y >= s->height) return;
    uint32_t words = s->pitch / 4u;
    s->fb[(uint64_t)y * words + (uint32_t)x] = disp_pack(s->fmt, r, g, b);
}

void disp_px_rgb(struct disp_state *s, int x, int y, uint32_t rgb) {
    if (!s->fb || x < 0 || y < 0 || (uint32_t)x >= s->width || (uint32_t)y >= s->height) return;
    uint32_t words = s->pitch / 4u;
    s->fb[(uint64_t)y * words + (uint32_t)x] = disp_pack_rgb(s->fmt, rgb);
}

void disp_blend_px(struct disp_state *s, int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t alpha) {
    if (!s->fb || x < 0 || y < 0 || (uint32_t)x >= s->width || (uint32_t)y >= s->height) return;
    if (alpha == 255) {
        disp_px(s, x, y, r, g, b);
        return;
    }
    if (alpha == 0) return;

    uint32_t words = s->pitch / 4u;
    volatile uint32_t *ptr = s->fb + ((uint64_t)y * words + (uint32_t)x);
    uint32_t old_px = *ptr;

    uint8_t br, bg, bb;
    disp_unpack(s->fmt, old_px, &br, &bg, &bb);

    uint32_t inv = 255 - alpha;
    uint8_t out_r = (uint8_t)((r * alpha + br * inv) / 255);
    uint8_t out_g = (uint8_t)((g * alpha + bg * inv) / 255);
    uint8_t out_b = (uint8_t)((b * alpha + bb * inv) / 255);

    *ptr = disp_pack(s->fmt, out_r, out_g, out_b);
}

void disp_blend_px_rgb(struct disp_state *s, int x, int y, uint32_t rgb, uint8_t alpha) {
    uint8_t r = (uint8_t)((rgb >> 16) & 0xFF);
    uint8_t g = (uint8_t)((rgb >> 8) & 0xFF);
    uint8_t b = (uint8_t)(rgb & 0xFF);
    disp_blend_px(s, x, y, r, g, b, alpha);
}

void disp_clear(struct disp_state *s, uint32_t rgb) {
    if (!s->fb) return;
    uint32_t val = disp_pack_rgb(s->fmt, rgb);
    uint32_t words = s->pitch / 4u;
    for (uint32_t y = 0; y < s->height; y++) {
        volatile uint32_t *row = s->fb + (uint64_t)y * words;
        for (uint32_t x = 0; x < s->width; x++) {
            row[x] = val;
        }
    }
}

void disp_fill_rect(struct disp_state *s, int x, int y, int w, int h, uint32_t rgb) {
    if (!s->fb) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)s->width) w = (int)s->width - x;
    if (y + h > (int)s->height) h = (int)s->height - y;
    if (w <= 0 || h <= 0) return;

    uint32_t val = disp_pack_rgb(s->fmt, rgb);
    uint32_t words = s->pitch / 4u;
    for (int j = 0; j < h; j++) {
        volatile uint32_t *row = s->fb + (uint64_t)(y + j) * words + x;
        for (int i = 0; i < w; i++) {
            row[i] = val;
        }
    }
}

void disp_fill_rect_alpha(struct disp_state *s, int x, int y, int w, int h, uint32_t rgb, uint8_t alpha) {
    if (alpha == 255) {
        disp_fill_rect(s, x, y, w, h, rgb);
        return;
    }
    if (alpha == 0 || !s->fb) return;
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            disp_blend_px_rgb(s, x + i, y + j, rgb, alpha);
        }
    }
}

void disp_rect_outline(struct disp_state *s, int x, int y, int w, int h, int thickness, uint32_t rgb) {
    if (thickness <= 0) return;
    disp_fill_rect(s, x, y, w, thickness, rgb);
    disp_fill_rect(s, x, y + h - thickness, w, thickness, rgb);
    disp_fill_rect(s, x, y, thickness, h, rgb);
    disp_fill_rect(s, x + w - thickness, y, thickness, h, rgb);
}

void disp_rounded_rect(struct disp_state *s, int x, int y, int w, int h, int r, uint32_t rgb, uint8_t alpha) {
    if (r <= 0) {
        disp_fill_rect_alpha(s, x, y, w, h, rgb, alpha);
        return;
    }
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;

    /* Внутреннее тело */
    disp_fill_rect_alpha(s, x + r, y, w - 2 * r, h, rgb, alpha);
    disp_fill_rect_alpha(s, x, y + r, r, h - 2 * r, rgb, alpha);
    disp_fill_rect_alpha(s, x + w - r, y + r, r, h - 2 * r, rgb, alpha);

    /* Закругление 4 углов */
    int r2 = r * r;
    for (int cy = 0; cy < r; cy++) {
        for (int cx = 0; cx < r; cx++) {
            int dx = cx - r + 1;
            int dy = cy - r + 1;
            if (dx * dx + dy * dy <= r2) {
                disp_blend_px_rgb(s, x + cx, y + cy, rgb, alpha);
                disp_blend_px_rgb(s, x + w - 1 - cx, y + cy, rgb, alpha);
                disp_blend_px_rgb(s, x + cx, y + h - 1 - cy, rgb, alpha);
                disp_blend_px_rgb(s, x + w - 1 - cx, y + h - 1 - cy, rgb, alpha);
            }
        }
    }
}

void disp_circle(struct disp_state *s, int cx, int cy, int r, uint32_t rgb, uint8_t alpha) {
    int r2 = r * r;
    for (int y = -r; y <= r; y++) {
        for (int x = -r; x <= r; x++) {
            if (x * x + y * y <= r2) {
                disp_blend_px_rgb(s, cx + x, cy + y, rgb, alpha);
            }
        }
    }
}

void disp_circle_outline(struct disp_state *s, int cx, int cy, int r, int thickness, uint32_t rgb, uint8_t alpha) {
    int r_out2 = r * r;
    int r_in = r - thickness;
    int r_in2 = (r_in > 0) ? (r_in * r_in) : 0;
    for (int y = -r; y <= r; y++) {
        for (int x = -r; x <= r; x++) {
            int d2 = x * x + y * y;
            if (d2 <= r_out2 && d2 >= r_in2) {
                disp_blend_px_rgb(s, cx + x, cy + y, rgb, alpha);
            }
        }
    }
}

void disp_hline(struct disp_state *s, int x, int y, int w, uint32_t rgb) {
    disp_fill_rect(s, x, y, w, 1, rgb);
}

void disp_vline(struct disp_state *s, int x, int y, int h, uint32_t rgb) {
    disp_fill_rect(s, x, y, 1, h, rgb);
}

void disp_draw_char(struct disp_state *s, char c, int x, int y, int scale, uint32_t rgb) {
    if (c < FONT_FIRST_CHAR || c > FONT_LAST_CHAR) return;
    const uint8_t *glyph = font8x16[(uint8_t)c - FONT_FIRST_CHAR];
    if (scale <= 1) {
        uint32_t val = disp_pack_rgb(s->fmt, rgb);
        uint32_t words = s->pitch / 4u;
        for (int gy = 0; gy < FONT_HEIGHT; gy++) {
            int py = y + gy;
            if (py < 0 || (uint32_t)py >= s->height) continue;
            uint8_t bits = glyph[gy];
            volatile uint32_t *row = s->fb + (uint64_t)py * words;
            for (int gx = 0; gx < FONT_WIDTH; gx++) {
                if ((bits >> (7 - gx)) & 1) {
                    int px = x + gx;
                    if (px >= 0 && (uint32_t)px < s->width) row[px] = val;
                }
            }
        }
    } else {
        for (int gy = 0; gy < FONT_HEIGHT; gy++) {
            uint8_t bits = glyph[gy];
            for (int gx = 0; gx < FONT_WIDTH; gx++) {
                if ((bits >> (7 - gx)) & 1) {
                    disp_fill_rect(s, x + gx * scale, y + gy * scale, scale, scale, rgb);
                }
            }
        }
    }
}

void disp_draw_text(struct disp_state *s, const char *text, int x, int y, int scale, uint32_t rgb) {
    if (!text) return;
    int cur_x = x;
    for (int i = 0; text[i]; i++) {
        if (text[i] == '\n') {
            y += FONT_HEIGHT * scale + 4;
            cur_x = x;
            continue;
        }
        disp_draw_char(s, text[i], cur_x, y, scale, rgb);
        cur_x += FONT_WIDTH * scale;
    }
}

int disp_text_width(const char *text, int scale) {
    if (!text) return 0;
    int n = 0;
    while (text[n] && text[n] != '\n') n++;
    return n * FONT_WIDTH * scale;
}

void disp_draw_text_center(struct disp_state *s, const char *text, int y, int scale, uint32_t rgb) {
    int w = disp_text_width(text, scale);
    int x = ((int)s->width - w) / 2;
    disp_draw_text(s, text, x, y, scale, rgb);
}

void disp_draw_hex(struct disp_state *s, uint64_t v, int x, int y, int scale, uint32_t rgb) {
    char buf[19];
    buf[0] = '0';
    buf[1] = 'x';
    static const char hex_digits[] = "0123456789ABCDEF";
    for (int i = 0; i < 16; i++) {
        int shift = (15 - i) * 4;
        buf[2 + i] = hex_digits[(v >> shift) & 0xF];
    }
    buf[18] = '\0';
    int start = 2;
    while (start < 17 && buf[start] == '0') start++;
    char trimmed[19];
    trimmed[0] = '0';
    trimmed[1] = 'x';
    int out = 2;
    for (int i = start; i < 18; i++) {
        trimmed[out++] = buf[i];
    }
    trimmed[out] = '\0';
    disp_draw_text(s, trimmed, x, y, scale, rgb);
}

void disp_draw_dec(struct disp_state *s, uint64_t v, int x, int y, int scale, uint32_t rgb) {
    char buf[24];
    if (v == 0) {
        disp_draw_text(s, "0", x, y, scale, rgb);
        return;
    }
    int len = 0;
    uint64_t tmp = v;
    while (tmp > 0) {
        len++;
        tmp /= 10;
    }
    buf[len] = '\0';
    for (int i = len - 1; i >= 0; i--) {
        buf[i] = '0' + (v % 10);
        v /= 10;
    }
    disp_draw_text(s, buf, x, y, scale, rgb);
}
