#include "a64.h"
#include "gs101_disp.h"

/* 
 * Упаковка пикселей под little-endian память AArch64 и контроллер IDMA (regs-dpp.h):
 * В little-endian памяти слово uint32_t val записывается как:
 *   байт[0] = val & 0xFF        (младший байт)
 *   байт[1] = (val >> 8) & 0xFF
 *   байт[2] = (val >> 16) & 0xFF
 *   байт[3] = (val >> 24) & 0xFF (старший байт)
 *
 * Если альфа A = 0xFF попадает в байт 0, контроллер дисплея читает его как
 * синий компонент 255, и чёрный цвет (0, 0, 0) превращается в ярко-синий!
 * Поэтому A (0xFF) должен быть в старшем байте (бит 24..31), а цветовые каналы — в байтах 0..2.
 */
uint32_t disp_pack(uint32_t fmt, uint8_t r, uint8_t g, uint8_t b) {
    uint32_t R = r, G = g, B = b, A = 0xFF;
    switch (fmt) {
    case 0: case 4:
        /* IDMA_IMG_FORMAT_BGRA8888 / BGRX8888: байт0=B, байт1=G, байт2=R, байт3=A */
        return (A << 24) | (R << 16) | (G << 8) | B;

    case 1: case 5:
        /* IDMA_IMG_FORMAT_RGBA8888 / RGBX8888: байт0=R, байт1=G, байт2=B, байт3=A */
        return (A << 24) | (B << 16) | (G << 8) | R;

    case 2: case 6:
        /* IDMA_IMG_FORMAT_ABGR8888 / XBGR8888: байт0=A, байт1=B, байт2=G, байт3=R */
        return (R << 24) | (G << 16) | (B << 8) | A;

    case 3: case 7:
        /* IDMA_IMG_FORMAT_ARGB8888 / XRGB8888: байт0=A, байт1=R, байт2=G, байт3=B */
        return (B << 24) | (G << 16) | (R << 8) | A;

    default:
        /* Стандартный fallback: BGRA8888 */
        return (A << 24) | (R << 16) | (G << 8) | B;
    }
}

uint32_t disp_pack_rgb(uint32_t fmt, uint32_t rgb) {
    uint8_t r = (uint8_t)((rgb >> 16) & 0xFF);
    uint8_t g = (uint8_t)((rgb >> 8) & 0xFF);
    uint8_t b = (uint8_t)(rgb & 0xFF);
    return disp_pack(fmt, r, g, b);
}

void disp_unpack(uint32_t fmt, uint32_t pixel, uint8_t *r, uint8_t *g, uint8_t *b) {
    switch (fmt) {
    case 0: case 4:
        *b = (uint8_t)(pixel & 0xFF);
        *g = (uint8_t)((pixel >> 8) & 0xFF);
        *r = (uint8_t)((pixel >> 16) & 0xFF);
        break;
    case 1: case 5:
        *r = (uint8_t)(pixel & 0xFF);
        *g = (uint8_t)((pixel >> 8) & 0xFF);
        *b = (uint8_t)((pixel >> 16) & 0xFF);
        break;
    case 2: case 6:
        *b = (uint8_t)((pixel >> 8) & 0xFF);
        *g = (uint8_t)((pixel >> 16) & 0xFF);
        *r = (uint8_t)((pixel >> 24) & 0xFF);
        break;
    case 3: case 7:
    default:
        *r = (uint8_t)((pixel >> 8) & 0xFF);
        *g = (uint8_t)((pixel >> 16) & 0xFF);
        *b = (uint8_t)((pixel >> 24) & 0xFF);
        break;
    }
}
