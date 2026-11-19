#ifndef NEXUS_GS101_DISP_DRAW_H
#define NEXUS_GS101_DISP_DRAW_H

#include "gs101_disp.h"

#define DISP_FONT_WIDTH  8
#define DISP_FONT_HEIGHT 16

/* Базовые пиксельные примитивы */
void disp_px(struct disp_state *s, int x, int y, uint8_t r, uint8_t g, uint8_t b);
void disp_px_rgb(struct disp_state *s, int x, int y, uint32_t rgb);
void disp_blend_px(struct disp_state *s, int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t alpha);
void disp_blend_px_rgb(struct disp_state *s, int x, int y, uint32_t rgb, uint8_t alpha);

/* Геометрические фигуры */
void disp_clear(struct disp_state *s, uint32_t rgb);
void disp_fill_rect(struct disp_state *s, int x, int y, int w, int h, uint32_t rgb);
void disp_fill_rect_alpha(struct disp_state *s, int x, int y, int w, int h, uint32_t rgb, uint8_t alpha);
void disp_rect_outline(struct disp_state *s, int x, int y, int w, int h, int thickness, uint32_t rgb);
void disp_rounded_rect(struct disp_state *s, int x, int y, int w, int h, int r, uint32_t rgb, uint8_t alpha);
void disp_circle(struct disp_state *s, int cx, int cy, int r, uint32_t rgb, uint8_t alpha);
void disp_circle_outline(struct disp_state *s, int cx, int cy, int r, int thickness, uint32_t rgb, uint8_t alpha);
void disp_hline(struct disp_state *s, int x, int y, int w, uint32_t rgb);
void disp_vline(struct disp_state *s, int x, int y, int h, uint32_t rgb);

/* Текст и форматирование */
void disp_draw_char(struct disp_state *s, char c, int x, int y, int scale, uint32_t rgb);
void disp_draw_text(struct disp_state *s, const char *text, int x, int y, int scale, uint32_t rgb);
int  disp_text_width(const char *text, int scale);
void disp_draw_text_center(struct disp_state *s, const char *text, int y, int scale, uint32_t rgb);
void disp_draw_hex(struct disp_state *s, uint64_t v, int x, int y, int scale, uint32_t rgb);
void disp_draw_dec(struct disp_state *s, uint64_t v, int x, int y, int scale, uint32_t rgb);

#endif
