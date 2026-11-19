#include "target.h"
/* NexusOS graphical desktop — version comes from nexus_version.h. */
#include <stdint.h>
#include "gui.h"
#include "font.h"
#include "mouse.h"
#include "pit.h"
#include "nexus_version.h"
#include "vfs.h"
#include "window.h"
#include "heap.h"
#include "paging.h"
#include "tsc.h"
#include "edid.h"
#include "display.h"
#include "power.h"
#include "process.h"
#include <string.h>
#include "nvme.h"
#include "ahci.h"

extern const uint8_t _binary_assets_wallpapers_nexus_default_rgb565_start[];
extern const uint8_t _binary_assets_wallpapers_nexus_default_rgb565_end[];
extern const uint8_t _binary_assets_wallpapers_wall1_nxw_start[];
extern const uint8_t _binary_assets_wallpapers_wall1_nxw_end[];
extern const uint8_t _binary_assets_wallpapers_wall2_nxw_start[];
extern const uint8_t _binary_assets_wallpapers_wall2_nxw_end[];
extern const uint8_t _binary_assets_wallpapers_wall3_nxw_start[];
extern const uint8_t _binary_assets_wallpapers_wall3_nxw_end[];
extern const uint8_t _binary_assets_wallpapers_wall4_nxw_start[];
extern const uint8_t _binary_assets_wallpapers_wall4_nxw_end[];
extern const uint8_t _binary_assets_wallpapers_wall5_nxw_start[];
extern const uint8_t _binary_assets_wallpapers_wall5_nxw_end[];
extern const uint8_t _binary_assets_cursors_cursor_bin_start[];
extern const uint8_t _binary_assets_cursors_cursor_bin_end[];

#define WALLPAPER_W 480
#define WALLPAPER_H 270

#define CUR_MAX    64       /* largest supported cursor image, in pixels */


static nexus_framebuffer_t *fb;
static int g_gui_active = 0;
static uint64_t g_fb_base;
static uint32_t g_fb_stride;
static int g_fb_bgr;
static int g_cursor_visible;
static int g_cursor_x, g_cursor_y;     /* mouse position the pointer was drawn for */
static int g_cursor_ox, g_cursor_oy;   /* top-left of the drawn rectangle (position - hotspot) */
static uint32_t g_cursor_saved[CUR_MAX * CUR_MAX]; /* only used without a backbuffer */
static uint8_t g_prev_buttons = 0;

/* Pointer image: RGBA, straight alpha. Comes from assets/cursors/cursor.bin
 * (built from cursor.png); falls back to a built-in arrow if the blob is bad. */
static const uint8_t *g_cur_rgba;
static int g_cur_w, g_cur_h, g_cur_hx, g_cur_hy;
static uint8_t g_cur_fallback[12 * 8 * 4];
static uint32_t *g_backbuffer;
static uint64_t g_backbuffer_bytes;
static uint32_t g_render_stride;

/* Frame pacing: the scene is re-rendered at most FRAME_US apart (60 Hz) and
 * redraw requests that arrive in between are coalesced into one frame. The
 * pointer is NOT paced - it is an overlay updated immediately (see below). */
static int g_scene_dirty = 1;
static uint32_t g_frame_count;

/* Selectable frame-rate limit (software pacing; UEFI GOP has no panel Hz control). */
#define HZ_MAX 8
static uint16_t g_hz_opts[HZ_MAX] = { 30, 60, 75, 120, 144, 240 };
static int g_hz_count = 6;
#define HZ_COUNT g_hz_count
static int g_hz_idx = 1;
static uint32_t g_panel_hz;             /* refresh rate read from the monitor's EDID (0 = unknown) */
static uint64_t g_frame_clk;            /* pacing-clock ticks per frame */
static uint64_t g_next_frame_clk;
static uint32_t g_fps, g_fps_frames;
static int g_show_fps = 0;              /* FPS counter is hidden unless the user turns it on */
static int g_locked = 0;                /* lock screen is showing */

/* Lock screen password/PIN state */
static char g_lock_password[32];
static int  g_lock_password_len = 0;
static char g_lock_input[32];
static int  g_lock_input_len = 0;
static int  g_lock_passwd_enabled = 0;  /* 0 = any key unlocks; 1 = PIN required */
static const char *g_lock_error = "";   /* error message, "" when none */

/* Unlock animation state (0..20 frame counter) */
static int g_unlock_anim = 0;
static int g_unlock_anim_active = 0;
static uint64_t g_unlock_anim_start = 0;


/* Pacing clock: the calibrated TSC when available (sub-microsecond), else the
 * 1 kHz PIT millisecond counter. */
static inline uint64_t pace_now(void) { return tsc_hz() ? tsc_read() : pit_get_uptime_ms(); }
static inline uint64_t pace_hz(void)  { return tsc_hz() ? tsc_hz() : 1000u; }

static inline void request_redraw(void) { g_scene_dirty = 1; }

static void gui_begin_unlock(void) {
    g_locked = 0;
    g_unlock_anim = 0;
    g_unlock_anim_start = pace_now();
    g_unlock_anim_active = 1;
    request_redraw();
}

static inline uint32_t pack(uint32_t rgb) {
    uint8_t r = (uint8_t)(rgb >> 16), g = (uint8_t)(rgb >> 8), b = (uint8_t)rgb;
    return g_fb_bgr ? ((uint32_t)b | ((uint32_t)g << 8) | ((uint32_t)r << 16))
                    : ((uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16));
}

static inline uint32_t blend_word(uint32_t old, uint32_t src, uint8_t alpha) {
    uint32_t ia = 255u - alpha;
    uint32_t rb = ((((old & 0x00FF00FFu) * ia) + ((src & 0x00FF00FFu) * alpha)) >> 8) & 0x00FF00FFu;
    uint32_t g  = ((((old & 0x0000FF00u) * ia) + ((src & 0x0000FF00u) * alpha)) >> 8) & 0x0000FF00u;
    return rb | g;
}

static inline uint32_t *fb_row(uint32_t y) {
    uint64_t base = g_backbuffer ? (uint64_t)(uintptr_t)g_backbuffer : g_fb_base;
    uint32_t stride = g_backbuffer ? g_render_stride : g_fb_stride;
    return (uint32_t *)(uintptr_t)(base + (uint64_t)y * stride);
}

/* Make sure freshly written framebuffer pixels actually reach the display.
 * With the framebuffer mapped write-combining only a store fence is needed;
 * if the mapping is still write-back the lines must be flushed explicitly,
 * otherwise small updates (the pointer) can sit in the CPU cache forever. */
static void fb_flush_rect(int x, int y, int w, int h) {
    if (!paging_fb_is_wc() && fb) {
        if (x < 0) { w += x; x = 0; }
        if (y < 0) { h += y; y = 0; }
        if (x + w > (int)fb->width)  w = (int)fb->width  - x;
        if (y + h > (int)fb->height) h = (int)fb->height - y;
        if (w <= 0 || h <= 0) { __asm__ volatile ("sfence" ::: "memory"); return; }
        for (int row = 0; row < h; ++row) {
            uintptr_t a = (uintptr_t)g_fb_base + (uint64_t)(y + row) * g_fb_stride + (uint64_t)x * 4u;
            uintptr_t end = a + (uintptr_t)w * 4u;
            for (a &= ~(uintptr_t)63; a < end; a += 64)
                __asm__ volatile ("clflush (%0)" : : "r"(a) : "memory");
        }
    }
    __asm__ volatile ("sfence" ::: "memory");
}

/* Copy a rectangle of the back buffer to the screen. Only the touched area
 * is transferred, so moving the pointer costs a few hundred bytes instead of
 * a whole 8 MiB frame. */
static void present_rect(int x, int y, int w, int h) {
    if (!fb || !g_backbuffer) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)fb->width)  w = (int)fb->width  - x;
    if (y + h > (int)fb->height) h = (int)fb->height - y;
    if (w <= 0 || h <= 0) return;

    uint8_t *dst = (uint8_t *)(uintptr_t)g_fb_base + (uint64_t)y * g_fb_stride + (uint64_t)x * 4u;
    const uint8_t *src = (const uint8_t *)(uintptr_t)g_backbuffer + (uint64_t)y * g_render_stride + (uint64_t)x * 4u;
    uint32_t bytes = (uint32_t)w * 4u;

    if (g_fb_stride == g_render_stride && bytes == g_fb_stride) {
        memcpy(dst, src, (size_t)bytes * (size_t)h);   /* one big burst */
    } else {
        for (int row = 0; row < h; ++row) {
            memcpy(dst + (uint64_t)row * g_fb_stride, src + (uint64_t)row * g_render_stride, bytes);
        }
    }
    fb_flush_rect(x, y, w, h);
}

static void gui_present(void) {
    if (fb) present_rect(0, 0, (int)fb->width, (int)fb->height);
}

static inline void px(int x, int y, uint32_t c) {
    if (!fb || x < 0 || y < 0 || (uint32_t)x >= fb->width || (uint32_t)y >= fb->height) return;
    fb_row((uint32_t)y)[x] = pack(c);
}

static inline void blend_px(int x, int y, uint32_t c, uint8_t alpha) {
    if (!fb || x < 0 || y < 0 || (uint32_t)x >= fb->width || (uint32_t)y >= fb->height) return;
    uint32_t *p = fb_row((uint32_t)y) + x;
    *p = blend_word(*p, pack(c), alpha);
}

static void rect(int x, int y, int w, int h, uint32_t c) {
    if (!fb || w <= 0 || h <= 0) return;
    uint32_t color = pack(c);
    for (int j = 0; j < h; ++j) {
        int yy = y + j;
        if (yy < 0 || (uint32_t)yy >= fb->height) continue;
        int x0 = x < 0 ? 0 : x, x1 = x + w;
        if ((uint32_t)x1 > fb->width) x1 = (int)fb->width;
        if (x0 >= x1) continue;
        uint32_t *row = fb_row((uint32_t)yy);
        for (int i = x0; i < x1; ++i) row[i] = color;
    }
}

static void rect_alpha(int x, int y, int w, int h, uint32_t c, uint8_t alpha) {
    if (!fb || w <= 0 || h <= 0) return;
    if (alpha == 255) { rect(x,y,w,h,c); return; }
    uint32_t src = pack(c);
    for (int j = 0; j < h; ++j) {
        int yy = y + j;
        if (yy < 0 || (uint32_t)yy >= fb->height) continue;
        int x0 = x < 0 ? 0 : x, x1 = x + w;
        if ((uint32_t)x1 > fb->width) x1 = (int)fb->width;
        if (x0 >= x1) continue;
        uint32_t *row = fb_row((uint32_t)yy);
        for (int i = x0; i < x1; ++i) row[i] = blend_word(row[i], src, alpha);
    }
}

static void rounded_rect_alpha(int x, int y, int w, int h, int r, uint32_t c, uint8_t alpha) {
    if (r < 1) { rect_alpha(x, y, w, h, c, alpha); return; }
    rect_alpha(x + r, y, w - 2 * r, h, c, alpha);
    rect_alpha(x, y + r, r, h - 2 * r, c, alpha);
    rect_alpha(x + w - r, y + r, r, h - 2 * r, c, alpha);
    for (int yy = 0; yy < r; ++yy) for (int xx = 0; xx < r; ++xx) {
        int dx = xx - r + 1, dy = yy - r + 1;
        if (dx * dx + dy * dy <= r * r) {
            blend_px(x + xx, y + yy, c, alpha);
            blend_px(x + w - 1 - xx, y + yy, c, alpha);
            blend_px(x + xx, y + h - 1 - yy, c, alpha);
            blend_px(x + w - 1 - xx, y + h - 1 - yy, c, alpha);
        }
    }
}

static void circle(int cx, int cy, int r, uint32_t c, uint8_t alpha) {
    int rr = r * r;
    for (int y = -r; y <= r; ++y) for (int x = -r; x <= r; ++x)
        if (x * x + y * y <= rr) blend_px(cx + x, cy + y, c, alpha);
}

static void glyph(int x, int y, char c, int scale, uint32_t color) {
    const uint8_t *g = gui_font_glyph(c);
    if (!g) return;
    if (scale == 1 && fb && x >= 0 && y >= 0 &&
        (uint32_t)x + 8u <= fb->width && (uint32_t)y + 16u <= fb->height) {
        uint32_t col = pack(color);
        for (int gy = 0; gy < 16; ++gy) {
            uint8_t bits = g[gy];
            if (!bits) continue;
            uint32_t *row = fb_row((uint32_t)(y + gy)) + x;
            for (int gx = 0; gx < 8; ++gx)
                if ((bits >> (7 - gx)) & 1) row[gx] = col;
        }
        return;
    }
    for (int gy = 0; gy < 16; ++gy)
        for (int gx = 0; gx < 8; ++gx)
            if ((g[gy] >> (7 - gx)) & 1)
                rect(x + gx * scale, y + gy * scale, scale, scale, color);
}

static void text_at(const char *s, int x, int y, int scale, uint32_t color) {
    for (int i = 0; s[i]; ++i) glyph(x + i * 8 * scale, y, s[i], scale, color);
}

static int text_width(const char *s, int scale) {
    int n = 0; while (s[n]) ++n; return n * 8 * scale;
}

static void text_center(const char *s, int y, int scale, uint32_t color) {
    text_at(s, ((int)fb->width - text_width(s, scale)) / 2, y, scale, color);
}

#define WALL_MAX_W 7680
#define WALL_MAX_H 4320
#define WALL_COUNT 6

/* Slot 0 = built-in raw 480x270 RGB565. Slots 1..5 = assets/wallpapers/wallN.png
 * converted by tools/make_wallpaper.py into "NXWP" blobs (u16 w, u16 h, RGB565). */
static const uint8_t *const g_wall_blob[WALL_COUNT] = {
    _binary_assets_wallpapers_nexus_default_rgb565_start,
    _binary_assets_wallpapers_wall1_nxw_start, _binary_assets_wallpapers_wall2_nxw_start,
    _binary_assets_wallpapers_wall3_nxw_start, _binary_assets_wallpapers_wall4_nxw_start,
    _binary_assets_wallpapers_wall5_nxw_start,
};
static const uint8_t *const g_wall_blob_end[WALL_COUNT] = {
    _binary_assets_wallpapers_nexus_default_rgb565_end,
    _binary_assets_wallpapers_wall1_nxw_end, _binary_assets_wallpapers_wall2_nxw_end,
    _binary_assets_wallpapers_wall3_nxw_end, _binary_assets_wallpapers_wall4_nxw_end,
    _binary_assets_wallpapers_wall5_nxw_end,
};

static uint16_t g_wall_x0[WALL_MAX_W];
static uint8_t  g_wall_fx[WALL_MAX_W];
static uint32_t *g_wall_cache;          /* wallpaper scaled + packed, built once per selection */
static uint32_t g_wall_cache_w, g_wall_cache_h;
static int g_wall_idx;                  /* selected wallpaper */
static int g_wall_built = -1;           /* wallpaper currently held by the cache */

static inline uint16_t wp_rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static int wall_get(int idx, const uint8_t **px_out, int *w, int *h) {
    if (idx < 0 || idx >= WALL_COUNT) return 0;
    const uint8_t *b = g_wall_blob[idx];
    size_t len = (size_t)(g_wall_blob_end[idx] - b);
    if (idx == 0) {
        if (len < (size_t)WALLPAPER_W * WALLPAPER_H * 2u) return 0;
        *px_out = b; *w = WALLPAPER_W; *h = WALLPAPER_H;
        return 1;
    }
    if (len < 8 || b[0] != 'N' || b[1] != 'X' || b[2] != 'W' || b[3] != 'P') return 0;
    int ww = wp_rd16(b + 4), hh = wp_rd16(b + 6);
    if (ww < 2 || hh < 2 || len < 8u + (size_t)ww * (size_t)hh * 2u) return 0;
    *px_out = b + 8; *w = ww; *h = hh;
    return 1;
}

#define WP_R8(v) ((uint32_t)((((v) >> 8) & 0xF8u) | ((v) >> 13)))
#define WP_G8(v) ((uint32_t)((((v) >> 3) & 0xFCu) | (((v) >> 9) & 3u)))
#define WP_B8(v) ((uint32_t)((((v) << 3) & 0xF8u) | (((v) >> 2) & 7u)))

static inline uint32_t wp_lerp(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t fx, uint32_t fy) {
    uint32_t top = a * (256u - fx) + b * fx;
    uint32_t bot = c * (256u - fx) + d * fx;
    return (top * (256u - fy) + bot * fy) >> 16;
}

/* Source coordinate (1/256 px) for destination pixel i of n, "cover" scaling. */
static inline int64_t wp_coord(uint32_t i, uint32_t n, uint64_t span, int64_t off, int64_t maxp) {
    int64_t p = off + (int64_t)(((uint64_t)i * span + span / 2u) / n) - 128;
    if (p < 0) p = 0;
    if (p > maxp) p = maxp;
    return p;
}

/* Scale the selected image to the screen once (bilinear, cover/centre-crop)
 * and pack it into the framebuffer pixel format. Each frame is then a memcpy. */
static void wallpaper_build_cache(void) {
    uint32_t w = fb->width, h = fb->height;
    if (g_wall_cache && (g_wall_cache_w != w || g_wall_cache_h != h)) {
        kfree(g_wall_cache);
        g_wall_cache = NULL;
    }
    if (!g_wall_cache) g_wall_cache = (uint32_t *)kmalloc((size_t)w * h * 4u);
    if (!g_wall_cache) return;
    g_wall_cache_w = w; g_wall_cache_h = h;
    g_wall_built = g_wall_idx;

    const uint8_t *src; int sw, sh;
    if (!wall_get(g_wall_idx, &src, &sw, &sh) && !wall_get(0, &src, &sw, &sh)) {
        memset(g_wall_cache, 0, (size_t)w * h * 4u);
        return;
    }

    uint64_t span_x, span_y;
    if ((uint64_t)w * (uint64_t)sh >= (uint64_t)h * (uint64_t)sw) {
        span_x = (uint64_t)sw << 8;
        span_y = (((uint64_t)h * (uint64_t)sw) << 8) / w;
    } else {
        span_y = (uint64_t)sh << 8;
        span_x = (((uint64_t)w * (uint64_t)sh) << 8) / h;
    }
    int64_t off_x = (int64_t)(((uint64_t)sw << 8) - span_x) / 2;
    int64_t off_y = (int64_t)(((uint64_t)sh << 8) - span_y) / 2;
    int64_t max_x = ((int64_t)(sw - 1)) << 8, max_y = ((int64_t)(sh - 1)) << 8;

    for (uint32_t x = 0; x < w; x++) {
        int64_t p = wp_coord(x, w, span_x, off_x, max_x);
        uint32_t x0 = (uint32_t)(p >> 8), fx = (uint32_t)(p & 255);
        if (x0 >= (uint32_t)(sw - 1)) { x0 = (uint32_t)(sw - 1); fx = 0; }
        g_wall_x0[x] = (uint16_t)x0; g_wall_fx[x] = (uint8_t)fx;
    }
    for (uint32_t y = 0; y < h; y++) {
        int64_t p = wp_coord(y, h, span_y, off_y, max_y);
        uint32_t y0 = (uint32_t)(p >> 8), fy = (uint32_t)(p & 255);
        if (y0 >= (uint32_t)(sh - 1)) { y0 = (uint32_t)(sh - 1); fy = 0; }
        uint32_t y1 = (y0 + 1 < (uint32_t)sh) ? y0 + 1 : y0;
        const uint8_t *r0 = src + (size_t)y0 * (size_t)sw * 2u;
        const uint8_t *r1 = src + (size_t)y1 * (size_t)sw * 2u;
        uint32_t *row = g_wall_cache + (size_t)y * w;
        for (uint32_t x = 0; x < w; x++) {
            uint32_t x0 = g_wall_x0[x], x1 = (x0 + 1 < (uint32_t)sw) ? x0 + 1 : x0, fx = g_wall_fx[x];
            uint16_t v00 = wp_rd16(r0 + x0 * 2u), v10 = wp_rd16(r0 + x1 * 2u);
            uint16_t v01 = wp_rd16(r1 + x0 * 2u), v11 = wp_rd16(r1 + x1 * 2u);
            uint32_t r = wp_lerp(WP_R8(v00), WP_R8(v10), WP_R8(v01), WP_R8(v11), fx, fy);
            uint32_t g = wp_lerp(WP_G8(v00), WP_G8(v10), WP_G8(v01), WP_G8(v11), fx, fy);
            uint32_t b = wp_lerp(WP_B8(v00), WP_B8(v10), WP_B8(v01), WP_B8(v11), fx, fy);
            row[x] = pack((r << 16) | (g << 8) | b);
        }
    }
}

static void draw_wallpaper(void) {
    if (!fb) return;
    if (fb->width > WALL_MAX_W || fb->height > WALL_MAX_H) {
        rect(0, 0, (int)fb->width, (int)fb->height, 0x000000);
        return;
    }
    if (!g_wall_cache || g_wall_cache_w != fb->width || g_wall_cache_h != fb->height || g_wall_built != g_wall_idx)
        wallpaper_build_cache();
    if (!g_wall_cache) { rect(0, 0, (int)fb->width, (int)fb->height, 0x000000); return; }

    if (g_backbuffer && g_render_stride == fb->width * 4u) {
        memcpy(g_backbuffer, g_wall_cache, (size_t)fb->width * fb->height * 4u);
    } else {
        for (uint32_t y = 0; y < fb->height; y++)
            memcpy(fb_row(y), g_wall_cache + (size_t)y * fb->width, (size_t)fb->width * 4u);
    }
}

/* Small nearest-neighbour preview of wallpaper `idx` (for the Settings picker). */
static void draw_wall_thumb(int x, int y, int tw, int th, int idx) {
    const uint8_t *src; int sw, sh;
    if (!wall_get(idx, &src, &sw, &sh)) { rect(x, y, tw, th, 0x2A2C33); return; }
    for (int j = 0; j < th; ++j) {
        const uint8_t *row = src + (size_t)((j * sh) / th) * (size_t)sw * 2u;
        for (int i = 0; i < tw; ++i) {
            uint16_t v = wp_rd16(row + (size_t)((i * sw) / tw) * 2u);
            px(x + i, y + j, (WP_R8(v) << 16) | (WP_G8(v) << 8) | WP_B8(v));
        }
    }
}

/* ---- Blur (frosted glass) -------------------------------------------------
 * Two-pass box blur, run twice (close to a gaussian), on a rectangle of the
 * back buffer. Used for the translucent taskbar and Start menu. */
static uint32_t *g_blur_tmp;
static size_t g_blur_tmp_px;
static uint32_t g_bs0[WALL_MAX_W], g_bs1[WALL_MAX_W], g_bs2[WALL_MAX_W];

static inline uint32_t blur_clamp(int i, int n) { return (uint32_t)(i < 0 ? 0 : (i >= n ? n - 1 : i)); }

static void box_row(const uint32_t *src, uint32_t *dst, int n, int r) {
    uint32_t inv = 65536u / (uint32_t)(2 * r + 1);
    uint32_t s0 = 0, s1 = 0, s2 = 0;
    for (int k = -r; k <= r; ++k) {
        uint32_t p = src[blur_clamp(k, n)];
        s0 += p & 255u; s1 += (p >> 8) & 255u; s2 += (p >> 16) & 255u;
    }
    for (int i = 0; i < n; ++i) {
        dst[i] = ((s0 * inv + 32768u) >> 16) | (((s1 * inv + 32768u) >> 16) << 8) | (((s2 * inv + 32768u) >> 16) << 16);
        uint32_t a = src[blur_clamp(i + r + 1, n)], b = src[blur_clamp(i - r, n)];
        s0 += (a & 255u) - (b & 255u);
        s1 += ((a >> 8) & 255u) - ((b >> 8) & 255u);
        s2 += ((a >> 16) & 255u) - ((b >> 16) & 255u);
    }
}

static void blur_rect(int x, int y, int w, int h, int r) {
    if (!fb || !g_backbuffer || r < 1) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)fb->width)  w = (int)fb->width - x;
    if (y + h > (int)fb->height) h = (int)fb->height - y;
    if (w < 4 || h < 4 || w > WALL_MAX_W) return;
    int rw = r > w / 2 ? w / 2 : r;
    int rh = r > h / 2 ? h / 2 : r;
    if (rw < 1) rw = 1;
    if (rh < 1) rh = 1;

    size_t need = (size_t)w * (size_t)h;
    if (g_blur_tmp_px < need) {
        if (g_blur_tmp) kfree(g_blur_tmp);
        g_blur_tmp = (uint32_t *)kmalloc(need * 4u);
        g_blur_tmp_px = g_blur_tmp ? need : 0;
        if (!g_blur_tmp) return;
    }

    uint32_t inv = 65536u / (uint32_t)(2 * rh + 1);
    for (int pass = 0; pass < 2; ++pass) {
        for (int j = 0; j < h; ++j)
            box_row(fb_row((uint32_t)(y + j)) + x, g_blur_tmp + (size_t)j * (size_t)w, w, rw);

        for (int i = 0; i < w; ++i) g_bs0[i] = g_bs1[i] = g_bs2[i] = 0;
        for (int k = -rh; k <= rh; ++k) {
            const uint32_t *row = g_blur_tmp + (size_t)blur_clamp(k, h) * (size_t)w;
            for (int i = 0; i < w; ++i) {
                uint32_t p = row[i];
                g_bs0[i] += p & 255u; g_bs1[i] += (p >> 8) & 255u; g_bs2[i] += (p >> 16) & 255u;
            }
        }
        for (int j = 0; j < h; ++j) {
            uint32_t *out = fb_row((uint32_t)(y + j)) + x;
            const uint32_t *ra = g_blur_tmp + (size_t)blur_clamp(j + rh + 1, h) * (size_t)w;
            const uint32_t *rb = g_blur_tmp + (size_t)blur_clamp(j - rh, h) * (size_t)w;
            for (int i = 0; i < w; ++i) {
                out[i] = ((g_bs0[i] * inv + 32768u) >> 16) | (((g_bs1[i] * inv + 32768u) >> 16) << 8) |
                         (((g_bs2[i] * inv + 32768u) >> 16) << 16);
                uint32_t a = ra[i], b = rb[i];
                g_bs0[i] += (a & 255u) - (b & 255u);
                g_bs1[i] += ((a >> 8) & 255u) - ((b >> 8) & 255u);
                g_bs2[i] += ((a >> 16) & 255u) - ((b >> 16) & 255u);
            }
        }
    }
}


/* ---- Pointer ------------------------------------------------------------
 * The pointer is an overlay drawn straight onto the screen. The back buffer
 * never contains it, so hiding it = re-copying the small rectangle underneath
 * from the back buffer. Moving the mouse therefore never forces a scene
 * redraw or a full-frame copy. */
static inline uint32_t *front_px(int x, int y) {
    return (uint32_t *)(uintptr_t)(g_fb_base + (uint64_t)y * g_fb_stride) + x;
}

static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

/* Built-in arrow, used only when cursor.bin is missing or malformed. */
static void cursor_use_fallback(void) {
    g_cur_w = 8; g_cur_h = 12; g_cur_hx = 0; g_cur_hy = 0;
    for (int y = 0; y < 12; y++) for (int x = 0; x < 8; x++) {
        uint8_t *p = &g_cur_fallback[(y * 8 + x) * 4];
        int on = x <= y / 2;
        uint8_t v = (x == 0) ? 0x1A : 0xFF;
        p[0] = v; p[1] = (x == 0) ? 0x16 : 0xFF; p[2] = (x == 0) ? 0x22 : 0xFF; p[3] = on ? 255 : 0;
    }
    g_cur_rgba = g_cur_fallback;
}

static void cursor_load(void) {
    const uint8_t *b = _binary_assets_cursors_cursor_bin_start;
    size_t len = (size_t)(_binary_assets_cursors_cursor_bin_end - _binary_assets_cursors_cursor_bin_start);
    g_cur_rgba = NULL;
    if (len >= 12 && b[0] == 'N' && b[1] == 'X' && b[2] == 'C' && b[3] == 'R') {
        int w = rd16(b + 4), h = rd16(b + 6), hx = rd16(b + 8), hy = rd16(b + 10);
        if (w >= 1 && h >= 1 && w <= CUR_MAX && h <= CUR_MAX &&
            len >= 12u + (size_t)w * (size_t)h * 4u) {
            g_cur_w = w; g_cur_h = h;
            g_cur_hx = hx < w ? hx : w - 1;
            g_cur_hy = hy < h ? hy : h - 1;
            g_cur_rgba = b + 12;
        }
    }
    if (!g_cur_rgba) cursor_use_fallback();
}

static void cursor_erase(void) {
    if (!g_cursor_visible || !fb) return;
    if (g_backbuffer) {
        present_rect(g_cursor_ox, g_cursor_oy, g_cur_w, g_cur_h);
    } else {
        for (int y = 0; y < g_cur_h; y++) {
            int sy = g_cursor_oy + y; if (sy < 0 || (uint32_t)sy >= fb->height) continue;
            for (int x = 0; x < g_cur_w; x++) {
                int sx = g_cursor_ox + x; if (sx < 0 || (uint32_t)sx >= fb->width) continue;
                *front_px(sx, sy) = g_cursor_saved[y * g_cur_w + x];
            }
        }
        fb_flush_rect(g_cursor_ox < 0 ? 0 : g_cursor_ox, g_cursor_oy < 0 ? 0 : g_cursor_oy, g_cur_w, g_cur_h);
    }
    g_cursor_visible = 0;
}

static void draw_cursor(void) {
    if (!fb) return;
    if (!g_cur_rgba) cursor_load();
    int mx = mouse_get_x(), my = mouse_get_y();
    cursor_erase();
    int ox = mx - g_cur_hx, oy = my - g_cur_hy;
    if (!g_backbuffer) {
        for (int y = 0; y < g_cur_h; y++) for (int x = 0; x < g_cur_w; x++) {
            int sx = ox + x, sy = oy + y;
            g_cursor_saved[y * g_cur_w + x] = (sx >= 0 && sy >= 0 && (uint32_t)sx < fb->width && (uint32_t)sy < fb->height)
                                              ? *front_px(sx, sy) : 0;
        }
    }
    for (int y = 0; y < g_cur_h; y++) {
        int sy = oy + y;
        if (sy < 0 || (uint32_t)sy >= fb->height) continue;
        for (int x = 0; x < g_cur_w; x++) {
            int sx = ox + x;
            if (sx < 0 || (uint32_t)sx >= fb->width) continue;
            const uint8_t *p = &g_cur_rgba[(y * g_cur_w + x) * 4];
            uint8_t a = p[3];
            if (!a) continue;
            uint32_t src = pack(((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2]);
            uint32_t *d = front_px(sx, sy);
            if (a == 255) *d = src;
            else {
                /* Blend against the clean scene, not against what is on screen. */
                uint32_t bg = g_backbuffer ? fb_row((uint32_t)sy)[sx] : *d;
                *d = blend_word(bg, src, a);
            }
        }
    }
    fb_flush_rect(ox < 0 ? 0 : ox, oy < 0 ? 0 : oy, g_cur_w, g_cur_h);
    g_cursor_x = mx; g_cursor_y = my; g_cursor_ox = ox; g_cursor_oy = oy; g_cursor_visible = 1;
}

/* ======================================================================
 * NexusOS Desktop 3.0 — Windows-style shell
 *   taskbar (Start, search, task buttons, tray, clock), Start menu,
 *   desktop icons, overlapping/draggable windows with min/max/close.
 * ====================================================================== */
#include "io.h"
#include "power.h"

/* ---- Theme ----------------------------------------------------------- */
static uint32_t g_accent = 0x5B9BFF;   /* user-selectable (Settings > Personalize) */
static uint32_t g_sel = 0x364F79;      /* selection colour derived from the accent */
#define C_ACCENT      g_accent
#define C_SEL         g_sel
#define C_TASKBAR     0x1B1C21
#define C_TB_HOVER    0x34363E
#define C_TB_ACTIVE   0x2A2C33
#define C_WIN_BODY    0x202128
#define C_WIN_TITLE_F 0x2F3139
#define C_WIN_TITLE_U 0x26272D
#define C_WIN_BORDER  0x3C3F49
#define C_TEXT        0xECEDF2
#define C_TEXT_DIM    0xA1A5B2
#define C_MENU        0x24252C
#define C_FIELD       0x30323B

/* ---- Apps ------------------------------------------------------------ */
#define NAPPS 5
static const struct { const char *name; gui_window_id_t id; int w, h; } g_apps[NAPPS] = {
    { "Files",      GUI_WINDOW_FILES,    660, 430 },
    { "Terminal",   GUI_WINDOW_TERMINAL, 640, 400 },
    { "Settings",   GUI_WINDOW_SETTINGS, 620, 450 },
    { "Calculator", GUI_WINDOW_CALC,     300, 420 },
    { "Notes",      GUI_WINDOW_NOTES,    460, 340 },
};
static int app_of_id(gui_window_id_t id) {
    for (int i = 0; i < NAPPS; ++i) if (g_apps[i].id == id) return i;
    return 2;
}

/* ---- State ----------------------------------------------------------- */
static int g_menu_open, g_menu_sel;
static char g_search_query[48];
static int g_search_len;
static int g_icon_sel = -1;
static int g_hover;
static int g_drag_id, g_drag_dx, g_drag_dy;
static uint64_t g_last_click_ms;
static int g_last_tag = -1;
static int g_files_item, g_files_top;
static int g_settings_item;
static uint64_t g_last_clock_second = (uint64_t)-1;
static int g_clock_h, g_clock_m, g_date_d, g_date_mo, g_date_y;
static const char *g_gui_message = "";
static uint64_t g_msg_until_ms;
static int tb_order[GUI_WINDOW_MAX], tb_count;

/* ---- Personalisation state ------------------------------------------ */
#define SET_CATS 5
#define ACCENT_COUNT 6
static const char *const g_set_cats[SET_CATS] = { "System", "Display", "Personalize", "Input", "About" };
static const uint32_t g_accents[ACCENT_COUNT] = { 0x5B9BFF, 0xB388FF, 0xFF7EB6, 0x4ADE80, 0xFFA94D, 0x2DD4BF };
static int g_accent_idx;
static int g_glass = 1;                 /* translucent + blurred taskbar / Start menu */
static int g_blur_lvl = 1;              /* 0 low, 1 medium, 2 high */
static const uint8_t g_blur_r[3] = { 4, 9, 16 };

static uint8_t gl(uint8_t a) { return g_glass ? a : 255; }

static void hz_set(int i) {
    if (i < 0 || i >= HZ_COUNT) return;
    g_hz_idx = i;
    g_frame_clk = pace_hz() / g_hz_opts[i];
    if (!g_frame_clk) g_frame_clk = 1;
    g_next_frame_clk = 0;
    request_redraw();
}
static void wall_select(int i) {
    if (i < 0 || i >= WALL_COUNT) return;
    g_wall_idx = i;
    request_redraw();
}
static void accent_set(int i) {
    if (i < 0 || i >= ACCENT_COUNT) return;
    g_accent_idx = i;
    g_accent = g_accents[i];
    uint32_t r = (g_accent >> 16) & 255u, g = (g_accent >> 8) & 255u, b = g_accent & 255u;
    r = (r * 38u + 0x20u * 62u) / 100u;
    g = (g * 38u + 0x21u * 62u) / 100u;
    b = (b * 38u + 0x28u * 62u) / 100u;
    g_sel = (r << 16) | (g << 8) | b;
    request_redraw();
}

#define TERM_LINES 64
#define TERM_COLS  100
static char g_term_lines[TERM_LINES][TERM_COLS];
static int g_term_count;
static char g_term_line[80];
static int g_term_len;

typedef struct { int x, y, w, h; } rc_t;
static inline int rc_hit(rc_t r, int x, int y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }

/* ---- Small helpers ---------------------------------------------------- */
static int streq(const char *a, const char *b) { int i = 0; while (a[i] && b[i] && a[i] == b[i]) i++; return a[i] == 0 && b[i] == 0; }
static int slen(const char *s) { int n = 0; while (s[n]) ++n; return n; }

static int fmt_u(char *out, uint32_t v) {
    char t[12]; int n = 0, k = 0;
    if (!v) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) out[k++] = t[--n];
    out[k] = 0;
    return k;
}
static int fmt_2d(char *out, int v) { out[0] = (char)('0' + (v / 10) % 10); out[1] = (char)('0' + v % 10); out[2] = 0; return 2; }

static void text_clip(const char *s, int x, int y, int maxw, uint32_t color) {
    int n = maxw / 8;
    for (int i = 0; s[i] && i < n; ++i) glyph(x + i * 8, y, s[i], 1, color);
}

static void frame(int x, int y, int w, int h, uint32_t c) {
    rect(x, y, w, 1, c); rect(x, y + h - 1, w, 1, c);
    rect(x, y, 1, h, c); rect(x + w - 1, y, 1, h, c);
}

static uint8_t cmos_rd(uint8_t r) { outb(0x70, r); return inb(0x71); }
static int bcd(int v) { return (v & 15) + (v >> 4) * 10; }

static void clock_refresh(void) {
    for (int i = 0; i < 20000 && (cmos_rd(0x0A) & 0x80); ++i) { }
    int m = cmos_rd(0x02), h = cmos_rd(0x04), d = cmos_rd(0x07), mo = cmos_rd(0x08), y = cmos_rd(0x09);
    int b = cmos_rd(0x0B);
    int pm = (h & 0x80) != 0; h &= 0x7F;
    if (!(b & 4)) { m = bcd(m); h = bcd(h); d = bcd(d); mo = bcd(mo); y = bcd(y); }
    if (!(b & 2)) { if (pm) h = (h % 12) + 12; else if (h == 12) h = 0; }
    g_clock_h = h % 24; g_clock_m = m % 60; g_date_d = d; g_date_mo = mo; g_date_y = 2000 + y;
}

/* ---- Icons ------------------------------------------------------------ */
static void icon_folder(int x, int y, int s) {
    rounded_rect_alpha(x, y + s / 6, s * 2 / 5, s / 4, 2, 0xD9A62E, 255);
    rounded_rect_alpha(x, y + s / 4, s, s * 3 / 4 - 1, 3, 0xE8B93C, 255);
    rect(x, y + s * 2 / 5, s, 1, 0xF6D77A);
}
static void icon_terminal(int x, int y, int s) {
    rounded_rect_alpha(x, y + 2, s, s - 4, 3, 0x14151A, 255);
    frame(x, y + 2, s, s - 4, 0x5A5E6B);
    if (s >= 24) text_at(">_", x + (s - 16) / 2, y + (s - 16) / 2, 1, 0x58D68D);
    else { rect(x + 4, y + s / 2 - 1, 5, 2, 0x58D68D); rect(x + 11, y + s / 2 + 2, 5, 2, 0x58D68D); }
}
static void icon_gear(int x, int y, int s, uint32_t hole) {
    int cx = x + s / 2, cy = y + s / 2, r = s * 3 / 10 + 1, r2 = r + 2, a = r2 * 7 / 10;
    static const int8_t dir[8][2] = { {1,0},{1,1},{0,1},{-1,1},{-1,0},{-1,-1},{0,-1},{1,-1} };
    for (int k = 0; k < 8; ++k) {
        int dx = dir[k][0] * ((dir[k][0] && dir[k][1]) ? a : r2);
        int dy = dir[k][1] * ((dir[k][0] && dir[k][1]) ? a : r2);
        rect(cx + dx - 2, cy + dy - 2, 4, 4, 0xB9BFCC);
    }
    circle(cx, cy, r, 0xB9BFCC, 255);
    circle(cx, cy, r / 2 + 1, hole, 255);
}
static void icon_calc(int x, int y, int s) {
    rounded_rect_alpha(x + s / 8, y, s * 3 / 4, s, 3, 0x4A4E5C, 255);
    rect(x + s / 4, y + s / 8, s / 2, s / 5, 0x9FE6B8);
    int k = s / 8; if (k < 2) k = 2;
    int gap = k / 3 + 1;
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c)
        rect(x + s / 4 + c * (k + gap), y + s * 2 / 5 + r * (k + gap), k, k, c == 2 ? 0xFFA94D : 0xD7DAE3);
}
static void icon_notes(int x, int y, int s) {
    rounded_rect_alpha(x + s / 6, y, s * 2 / 3, s, 3, 0xF2D95C, 255);
    rect(x + s / 6, y, s * 2 / 3, s / 6 + 1, 0xD9B93A);
    for (int i = 0; i < 3; ++i)
        rect(x + s / 6 + s / 8, y + s / 3 + i * (s / 5), s * 2 / 3 - s / 4, s >= 24 ? 2 : 1, 0x8A7424);
}
static void app_icon(int idx, int x, int y, int s, uint32_t hole) {
    if (idx == 0) icon_folder(x, y, s);
    else if (idx == 1) icon_terminal(x, y, s);
    else if (idx == 3) icon_calc(x, y, s);
    else if (idx == 4) icon_notes(x, y, s);
    else icon_gear(x, y, s, hole);
}
static void icon_nexus(int cx, int cy, uint32_t color) {
    for (int y = -10; y <= 10; ++y) for (int x = -10; x <= 10; ++x) {
        int d = x * x + y * y;
        if (d >= 42 && d <= 100) blend_px(cx + x, cy + y, color, 255);
    }
    circle(cx, cy, 3, color, 255);
}
static void icon_search(int cx, int cy, uint32_t color) {
    for (int y = -6; y <= 6; ++y) for (int x = -6; x <= 6; ++x) {
        int d = x * x + y * y;
        if (d >= 20 && d <= 40) blend_px(cx - 2 + x, cy - 2 + y, color, 255);
    }
    for (int i = 0; i < 6; ++i) { px(cx + 3 + i, cy + 3 + i, color); px(cx + 4 + i, cy + 3 + i, color); }
}
static void icon_net(int x, int y, uint32_t c) { for (int i = 0; i < 4; ++i) rect(x + i * 4, y + 12 - i * 4, 3, 4 + i * 4, c); }
static void icon_speaker(int x, int y, uint32_t c) {
    rect(x, y + 5, 4, 6, c);
    for (int i = 0; i < 6; ++i) rect(x + 4 + i, y + 5 - i / 1 + (i > 3 ? 1 : 0), 1, 6 + 2 * i - (i > 3 ? 2 : 0), c);
    for (int i = -3; i <= 3; ++i) px(x + 13 + (i * i) / 5, y + 8 + i, c);
}

/* ---- Geometry --------------------------------------------------------- */
static int tb_h(void) { return fb->height >= 900 ? 48 : 40; }
static int tb_top(void) { return (int)fb->height - tb_h(); }
static rc_t r_start(void) { return (rc_t){ 0, tb_top(), tb_h() + 6, tb_h() }; }
static rc_t r_search(void) {
    rc_t s = r_start();
    int w = fb->width >= 900 ? 200 : 0;
    return (rc_t){ s.w + 6, tb_top() + 6, w, tb_h() - 12 };
}
#define CLOCK_W 92
static rc_t r_clock(void) { return (rc_t){ (int)fb->width - 12 - CLOCK_W - 4, tb_top(), CLOCK_W + 4, tb_h() }; }
static rc_t r_tray(void) { rc_t c = r_clock(); return (rc_t){ c.x - 56, tb_top(), 56, tb_h() }; }
static rc_t r_desktop_btn(void) { return (rc_t){ (int)fb->width - 8, tb_top(), 8, tb_h() }; }

static void tb_sync(void) {
    int j = 0;
    for (int i = 0; i < tb_count; ++i) if (gui_window_get((gui_window_id_t)tb_order[i])) tb_order[j++] = tb_order[i];
    tb_count = j;
    for (int id = 1; id < GUI_WINDOW_MAX; ++id) {
        if (!gui_window_get((gui_window_id_t)id)) continue;
        int found = 0;
        for (int i = 0; i < tb_count; ++i) if (tb_order[i] == id) found = 1;
        if (!found && tb_count < GUI_WINDOW_MAX) tb_order[tb_count++] = id;
    }
}

static rc_t r_task(int i) {
    rc_t s = r_search();
    int x0 = (s.w ? s.x + s.w : r_start().w) + 10;
    int x1 = r_tray().x - 8;
    int n = tb_count ? tb_count : 1;
    int bw = (x1 - x0) / n - 4;
    if (bw > 170) bw = 170;
    if (bw < 44) bw = 44;
    return (rc_t){ x0 + i * (bw + 4), tb_top() + 4, bw, tb_h() - 8 };
}

static rc_t r_menu(void) {
    int w = (int)fb->width - 16; if (w > 400) w = 400;
    int h = tb_top() - 20; if (h > 500) h = 500;
    return (rc_t){ 8, tb_top() - h - 8, w, h };
}
static rc_t r_menu_search(void) { rc_t m = r_menu(); return (rc_t){ m.x + 16, m.y + 16, m.w - 32, 36 }; }
static rc_t r_menu_row(int i) { rc_t m = r_menu(); return (rc_t){ m.x + 10, m.y + 96 + i * 54, m.w - 20, 50 }; }
static rc_t r_btn_shutdown(void) { rc_t m = r_menu(); return (rc_t){ m.x + m.w - 16 - 96, m.y + m.h - 48, 96, 36 }; }
static rc_t r_btn_reboot(void) { rc_t b = r_btn_shutdown(); return (rc_t){ b.x - 8 - 84, b.y, 84, 36 }; }
static rc_t r_btn_lock(void) { rc_t b = r_btn_reboot(); return (rc_t){ b.x - 8 - 64, b.y, 64, 36 }; }
extern const uint8_t _binary_assets_icons_files_rgba_start[],    _binary_assets_icons_files_rgba_end[];
extern const uint8_t _binary_assets_icons_terminal_rgba_start[], _binary_assets_icons_terminal_rgba_end[];
extern const uint8_t _binary_assets_icons_settings_rgba_start[], _binary_assets_icons_settings_rgba_end[];
extern const uint8_t _binary_assets_icons_calc_rgba_start[],     _binary_assets_icons_calc_rgba_end[];
extern const uint8_t _binary_assets_icons_notes_rgba_start[],    _binary_assets_icons_notes_rgba_end[];

/* порядок как в g_apps: Files, Terminal, Settings, Calculator, Notes */
static const uint8_t *const g_icon_png[NAPPS]     = { _binary_assets_icons_files_rgba_start, _binary_assets_icons_terminal_rgba_start,
                                                      _binary_assets_icons_settings_rgba_start, _binary_assets_icons_calc_rgba_start,
                                                      _binary_assets_icons_notes_rgba_start };
static const uint8_t *const g_icon_png_end[NAPPS] = { _binary_assets_icons_files_rgba_end, _binary_assets_icons_terminal_rgba_end,
                                                      _binary_assets_icons_settings_rgba_end, _binary_assets_icons_calc_rgba_end,
                                                      _binary_assets_icons_notes_rgba_end };
/* вертикальные слоты как на прототипе: Files, Settings, Terminal, Notes, Calculator */
static const int g_slot[NAPPS] = { 0, 2, 1, 4, 3 };

#define ICON_TILE 64
#define ICON_PITCH 97
#define ICON_X 29
#define ICON_Y 29

static rc_t r_icon(int i) { return (rc_t){ ICON_X - 8, ICON_Y + g_slot[i] * ICON_PITCH - 6, 80, 92 }; }

static void blit_icon(int i, int x0, int y0) {
    const uint8_t *s = g_icon_png[i];
    if ((size_t)(g_icon_png_end[i] - s) != (size_t)ICON_TILE * ICON_TILE * 4) {   /* нет/битая иконка */
        rounded_rect_alpha(x0, y0, ICON_TILE, ICON_TILE, 14, 0xFFFFFF, 255);
        app_icon(i, x0 + 16, y0 + 16, 32, 0xFFFFFF);
        return;
    }
    for (int y = 0; y < ICON_TILE; ++y) {
        int sy = y0 + y; if (sy < 0 || sy >= (int)fb->height) continue;
        uint32_t *row = fb_row((uint32_t)sy);
        for (int x = 0; x < ICON_TILE; ++x) {
            int sx = x0 + x; if (sx < 0 || sx >= (int)fb->width) continue;
            const uint8_t *p = s + ((size_t)y * ICON_TILE + x) * 4;
            if (!p[3]) continue;
            uint32_t c = pack(((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2]);
            row[sx] = p[3] == 255 ? c : blend_word(row[sx], c, p[3]);
        }
    }
}

static void draw_desktop_icons(void) {
    for (int i = 0; i < NAPPS; ++i) {
        rc_t r = r_icon(i);
        int x = ICON_X, y = ICON_Y + g_slot[i] * ICON_PITCH;
        int sel = (i == g_icon_sel), hov = (g_hover == 300 + i);
        if (sel || hov) rounded_rect_alpha(r.x, r.y, r.w, r.h, 12, 0xFFFFFF, sel ? 70 : 38);
        blit_icon(i, x, y);
        int tx = x + (ICON_TILE - text_width(g_apps[i].name, 1)) / 2;
        text_at(g_apps[i].name, tx + 1, y + ICON_TILE + 9, 1, 0x000000);   /* тень */
        text_at(g_apps[i].name, tx,     y + ICON_TILE + 8, 1, 0xFFFFFF);
    }
}

static int win_btn_hit(const gui_window_t *w, int mx, int my) {   /* 0 none, 1 min, 2 max, 3 close */
    if (my < w->y || my >= w->y + 32) return 0;
    if (mx >= w->x + w->width - 46) return 3;
    if (mx >= w->x + w->width - 92) return 2;
    if (mx >= w->x + w->width - 138) return 1;
    return 0;
}

/* ---- Search / menu list ---------------------------------------------- */
static int search_match(const char *name) {
    if (!g_search_len) return 1;
    for (int i = 0; name[i]; ++i) {
        int j = 0;
        while (g_search_query[j] && name[i + j]) {
            char a = name[i + j], b = g_search_query[j];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) break;
            ++j;
        }
        if (!g_search_query[j]) return 1;
    }
    return 0;
}
static int menu_list(int out[NAPPS]) {
    int n = 0;
    for (int i = 0; i < NAPPS; ++i) if (search_match(g_apps[i].name)) out[n++] = i;
    return n;
}

/* ---- Calculator (fixed point, 3 decimals) ------------------------------- */
#define CALC_SCALE 1000
#define CALC_LIM   900000000000000000LL
static char g_calc_buf[20];
static int g_calc_blen;
static int64_t g_calc_acc;
static int g_calc_op, g_calc_fresh, g_calc_err;

static const char g_calc_lbl[5][4][4] = {
    { "C", "+/-", "%", "/" }, { "7", "8", "9", "x" }, { "4", "5", "6", "-" },
    { "1", "2", "3", "+" },   { "0", ".", "<", "=" },
};
static const char g_calc_code[5][4] = {
    { 'C', 'N', '%', '/' }, { '7', '8', '9', '*' }, { '4', '5', '6', '-' },
    { '1', '2', '3', '+' }, { '0', '.', 'B', '=' },
};

static int fmt_u64(char *out, uint64_t v) {
    char t[24]; int n = 0, k = 0;
    if (!v) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) out[k++] = t[--n];
    out[k] = 0;
    return k;
}

static int calc_fmt(char *out, int64_t v) {
    int n = 0;
    if (v < 0) { out[n++] = '-'; v = -v; }
    n += fmt_u64(out + n, (uint64_t)(v / CALC_SCALE));
    int fp = (int)(v % CALC_SCALE);
    if (fp) {
        out[n++] = '.';
        char d[3] = { (char)('0' + fp / 100), (char)('0' + (fp / 10) % 10), (char)('0' + fp % 10) };
        int len = 3;
        while (len > 1 && d[len - 1] == '0') --len;
        for (int i = 0; i < len; ++i) out[n++] = d[i];
    }
    out[n] = 0;
    return n;
}

static int64_t calc_parse(void) {
    int neg = 0, i = 0, fd = 0;
    int64_t ip = 0, fp = 0;
    if (g_calc_blen > 0 && g_calc_buf[0] == '-') { neg = 1; i = 1; }
    for (; i < g_calc_blen && g_calc_buf[i] != '.'; ++i) ip = ip * 10 + (g_calc_buf[i] - '0');
    if (i < g_calc_blen && g_calc_buf[i] == '.')
        for (++i; i < g_calc_blen && fd < 3; ++i, ++fd) fp = fp * 10 + (g_calc_buf[i] - '0');
    while (fd < 3) { fp *= 10; ++fd; }
    int64_t v = ip * CALC_SCALE + fp;
    return neg ? -v : v;
}

static void calc_clear(void) {
    g_calc_blen = 0; g_calc_buf[0] = 0; g_calc_acc = 0;
    g_calc_op = 0; g_calc_fresh = 0; g_calc_err = 0;
}

static int64_t calc_apply(int64_t a, int op, int64_t b) {
    int64_t r = 0;
    if (op == '+') r = a + b;
    else if (op == '-') r = a - b;
    else if (op == '*') {
        int64_t aa = a < 0 ? -a : a, bb = b < 0 ? -b : b;
        if (aa && bb > 9000000000000000000LL / aa) { g_calc_err = 1; return 0; }
        r = (a * b) / CALC_SCALE;
    } else if (op == '/') {
        int64_t aa = a < 0 ? -a : a;
        if (b == 0 || aa > 9000000000000000LL) { g_calc_err = 1; return 0; }
        r = (a * CALC_SCALE) / b;
    } else r = b;
    if (r > CALC_LIM || r < -CALC_LIM) { g_calc_err = 1; return 0; }
    return r;
}

static void calc_operate(int newop) {
    if (g_calc_blen > 0) {
        int64_t v = calc_parse();
        g_calc_acc = g_calc_op ? calc_apply(g_calc_acc, g_calc_op, v) : v;
    }
    g_calc_blen = 0; g_calc_buf[0] = 0;
    g_calc_op = g_calc_err ? 0 : newop;
    g_calc_fresh = 1;
}

static void calc_press(int k) {
    if (g_calc_err && k != 'C') {
        if (!((k >= '0' && k <= '9') || k == '.')) return;
        calc_clear();
    }
    if (k == 'C') { calc_clear(); return; }
    if (k >= '0' && k <= '9') {
        if (g_calc_blen == 1 && g_calc_buf[0] == '0') g_calc_blen = 0;
        if (g_calc_blen < 15) { g_calc_buf[g_calc_blen++] = (char)k; g_calc_buf[g_calc_blen] = 0; }
        return;
    }
    if (k == '.') {
        for (int i = 0; i < g_calc_blen; ++i) if (g_calc_buf[i] == '.') return;
        if (g_calc_blen == 0) { g_calc_buf[g_calc_blen++] = '0'; }
        if (g_calc_blen < 15) { g_calc_buf[g_calc_blen++] = '.'; g_calc_buf[g_calc_blen] = 0; }
        return;
    }
    if (k == 'B') {
        if (g_calc_blen > 0) g_calc_buf[--g_calc_blen] = 0;
        if (g_calc_blen == 1 && g_calc_buf[0] == '-') { g_calc_blen = 0; g_calc_buf[0] = 0; }
        return;
    }
    if (k == 'N') {
        if (g_calc_blen > 0) {
            if (g_calc_buf[0] == '-') {
                for (int i = 0; i < g_calc_blen; ++i) g_calc_buf[i] = g_calc_buf[i + 1];
                --g_calc_blen;
            } else if (g_calc_blen < 15) {
                for (int i = g_calc_blen; i >= 0; --i) g_calc_buf[i + 1] = g_calc_buf[i];
                g_calc_buf[0] = '-'; ++g_calc_blen;
            }
        } else g_calc_acc = -g_calc_acc;
        return;
    }
    if (k == '%') {
        if (g_calc_blen > 0) g_calc_blen = calc_fmt(g_calc_buf, calc_parse() / 100);
        else g_calc_acc /= 100;
        return;
    }
    if (k == '=') { calc_operate(0); return; }
    if (k == '+' || k == '-' || k == '*' || k == '/') calc_operate(k);
}

static void calc_display(char *out) {
    if (g_calc_err) { const char *e = "Error"; int i = 0; while (e[i]) { out[i] = e[i]; ++i; } out[i] = 0; }
    else if (g_calc_blen > 0) { int i = 0; while (g_calc_buf[i]) { out[i] = g_calc_buf[i]; ++i; } out[i] = 0; }
    else calc_fmt(out, g_calc_acc);
}

static rc_t calc_btn(const gui_window_t *w, int r, int c) {
    int gx = w->x + 12, gw = w->width - 24;
    int gy = w->y + 32 + 12 + 64 + 12, gh = w->y + w->height - 12 - gy;
    int bw = (gw - 3 * 8) / 4, bh = (gh - 4 * 8) / 5;
    if (bw < 8) bw = 8;
    if (bh < 8) bh = 8;
    return (rc_t){ gx + c * (bw + 8), gy + r * (bh + 8), bw, bh };
}

/* ---- Notes ----------------------------------------------------------------- */
#define NOTE_CAP (VFS_CONTENT_LEN - 1)
static char g_note[NOTE_CAP + 1];
static int g_note_len;

static void notes_load(void) {
    int n = vfs_read("note.txt", g_note, NOTE_CAP + 1);
    g_note_len = n > 0 ? n : 0;
    g_note[g_note_len] = 0;
}
static rc_t note_save_btn(const gui_window_t *w)  { return (rc_t){ w->x + 12,  w->y + w->height - 40, 84, 28 }; }
static rc_t note_clear_btn(const gui_window_t *w) { return (rc_t){ w->x + 104, w->y + w->height - 40, 84, 28 }; }
static void notes_save(void) {
    g_note[g_note_len] = 0;
    gui_status(vfs_write("note.txt", g_note) == 0 ? "Saved note.txt" : "Could not save note.txt");
}

/* ---- Terminal --------------------------------------------------------- */
static void term_print(const char *s) {
    if (g_term_count == TERM_LINES) {
        for (int i = 1; i < TERM_LINES; ++i) memcpy(g_term_lines[i - 1], g_term_lines[i], TERM_COLS);
        g_term_count--;
    }
    int n = 0;
    while (s[n] && n < TERM_COLS - 1) { g_term_lines[g_term_count][n] = s[n]; ++n; }
    g_term_lines[g_term_count][n] = 0;
    g_term_count++;
}
static void term_reset(void) {
    g_term_count = 0; g_term_len = 0; g_term_line[0] = 0;
    term_print("NexusOS " NEXUS_VERSION_DISPLAY " - Terminal");
    term_print("Type 'help' for a list of commands.");
}

/* Shows a full-screen notice, then really powers the machine off / resets it.
 * Never returns. */
static void power_action(int reboot) {
    g_menu_open = 0;
    if (fb) {
        draw_wallpaper();
        rect_alpha(0, 0, (int)fb->width, (int)fb->height, 0x000000, 170);
        text_center(reboot ? "Restarting..." : "Shutting down...", (int)fb->height / 2 - 16, 2, 0xFFFFFF);
        gui_present();
    }
    uint64_t t0 = pit_get_uptime_ms();
    while (pit_get_uptime_ms() - t0 < 400) __asm__ volatile ("pause");
    if (reboot) nexus_power_reboot();
    nexus_power_shutdown();
}

static int ci_eq(const char *a, const char *b) {
    for (;; ++a, ++b) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return 0;
        if (!x) return 1;
    }
}

static void close_all_windows(void) {
    for (int i = 0; i < NAPPS; ++i)
        if (gui_window_get(g_apps[i].id)) gui_window_close(g_apps[i].id);
}

/* Leave the graphical desktop completely (back to the command line). */
static void kill_gui(void) {
    close_all_windows();
    g_menu_open = 0;
    g_locked = 0;
    gui_exit();
}

static void launch_app(int idx) {
    if (idx < 0 || idx >= NAPPS) return;
    int was_open = gui_window_get(g_apps[idx].id) != 0;
    gui_window_open(g_apps[idx].id, g_apps[idx].name, g_apps[idx].w, g_apps[idx].h);
    if (!was_open) {
        if (g_apps[idx].id == GUI_WINDOW_TERMINAL) term_reset();
        if (g_apps[idx].id == GUI_WINDOW_FILES) { g_files_item = 0; g_files_top = 0; }
        if (g_apps[idx].id == GUI_WINDOW_SETTINGS) g_settings_item = 0;
        if (g_apps[idx].id == GUI_WINDOW_CALC) calc_clear();
        if (g_apps[idx].id == GUI_WINDOW_NOTES) notes_load();
    }
}

static const char *cmd_arg(const char *c, const char *name) {
    int i = 0;
    while (name[i]) { if (c[i] != name[i]) return 0; ++i; }
    if (c[i] == 0) return c + i;
    if (c[i] != ' ') return 0;
    while (c[i] == ' ') ++i;
    return c + i;
}
static int parse_u(const char *s) {
    if (*s < '0' || *s > '9') return -1;
    int v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); ++s; }
    return v;
}

/* Extra terminal commands. Returns 1 if the line was handled. */
static int term_extra(const char *c) {
    const char *a; char b[64]; int n;
    if (streq(c, "files")) { launch_app(0); return 1; }
    if (streq(c, "terminal")) { launch_app(1); return 1; }
    if (streq(c, "settings")) { launch_app(2); return 1; }
    if (streq(c, "calc")) { launch_app(3); return 1; }
    if (streq(c, "notes")) { launch_app(4); return 1; }
    if ((a = cmd_arg(c, "fps")) != 0) {
        if (streq(a, "on")) { g_show_fps = 1; request_redraw(); term_print("FPS counter on."); return 1; }
        if (streq(a, "off")) { g_show_fps = 0; request_redraw(); term_print("FPS counter off."); return 1; }
        if (streq(a, "toggle")) { g_show_fps = !g_show_fps; request_redraw(); term_print(g_show_fps ? "FPS counter on." : "FPS counter off."); return 1; }
        n = 0; const char *l = "Render rate: "; while (l[n]) { b[n] = l[n]; ++n; }
        n += fmt_u(b + n, g_fps); const char *u = " fps, limit "; for (int i = 0; u[i]; ++i) b[n++] = u[i];
        n += fmt_u(b + n, g_hz_opts[g_hz_idx]); b[n++] = ' '; b[n++] = 'H'; b[n++] = 'z'; b[n] = 0;
        term_print(b);
        term_print("Usage: fps [on|off|toggle]");
        return 1;
    }
    if (streq(c, "lock")) { g_locked = 1; request_redraw(); return 1; }
    if (streq(c, "reboot") || streq(c, "restart")) { power_action(1); return 1; }
    if (streq(c, "shutdown") || streq(c, "poweroff")) { power_action(0); return 1; }
    if (streq(c, "exit gui") || streq(c, "quit")) { kill_gui(); return 1; }
    if (streq(c, "ps") || streq(c, "apps")) {
        int any = 0;
        term_print("Open windows:");
        for (int i = 0; i < NAPPS; ++i)
            if (gui_window_get(g_apps[i].id)) { n = 0; b[n++] = ' '; b[n++] = ' '; for (const char *q = g_apps[i].name; *q; ++q) b[n++] = *q; b[n] = 0; term_print(b); any = 1; }
        if (!any) term_print("  (none)");
        term_print("Processes:");
        any = 0;
        for (uint64_t pid = 1; pid < 256; ++pid) {
            nexus_process_t *p = process_get(pid);
            if (!p || p->state == PROCESS_UNUSED || p->state == PROCESS_ZOMBIE) continue;
            n = 0; b[n++] = ' '; b[n++] = ' ';
            n += fmt_u(b + n, (uint32_t)p->pid); b[n++] = ' ';
            for (const char *q = p->name; *q && n < 50; ++q) b[n++] = *q;
            b[n] = 0; term_print(b); any = 1;
        }
        if (!any) term_print("  (none)");
        return 1;
    }
    if ((a = cmd_arg(c, "kill")) != 0) {
        if (!*a) { term_print("Usage: kill <app name | pid | all | gui>"); return 1; }
        if (ci_eq(a, "gui")) { kill_gui(); return 1; }
        if (ci_eq(a, "all")) { close_all_windows(); return 1; }
        int v = parse_u(a);
        if (v >= 0) {
            nexus_process_t *p = process_get((uint64_t)v);
            if (!p || p->state == PROCESS_UNUSED || p->state == PROCESS_ZOMBIE) { term_print("No such process."); return 1; }
            if (p == process_current()) { term_print("Refusing to kill the running process."); return 1; }
            term_print(process_exit((uint64_t)v) ? "Process terminated." : "Could not terminate process.");
            return 1;
        }
        for (int i = 0; i < NAPPS; ++i) if (ci_eq(a, g_apps[i].name)) {
            if (gui_window_get(g_apps[i].id)) { gui_window_close(g_apps[i].id); term_print("Closed."); }
            else term_print("Not running.");
            return 1;
        }
        term_print("No such app or process.");
        return 1;
    }
    if (streq(c, "date")) {
        n = 0; n += fmt_2d(b + n, g_date_d); b[n++] = '.'; n += fmt_2d(b + n, g_date_mo); b[n++] = '.';
        n += fmt_u(b + n, (uint32_t)g_date_y); b[n++] = ' ';
        n += fmt_2d(b + n, g_clock_h); b[n++] = ':'; n += fmt_2d(b + n, g_clock_m); b[n] = 0;
        term_print(b); return 1;
    }
    if ((a = cmd_arg(c, "hz")) != 0) {
        int v = parse_u(a);
        for (int i = 0; i < HZ_COUNT; ++i) if (v == g_hz_opts[i]) { hz_set(i); term_print("Frame limit set."); return 1; }
        { char sb[64]; int k = 0; const char *l = "Supported:"; while (l[k]) { sb[k] = l[k]; ++k; }
          for (int i = 0; i < g_hz_count; ++i) { sb[k++] = ' '; k += fmt_u(sb + k, g_hz_opts[i]); }
          sb[k] = 0; term_print(sb); }
        return 1;
    }
    if ((a = cmd_arg(c, "wall")) != 0) {
        int v = parse_u(a);
        if (v >= 0 && v < WALL_COUNT) { wall_select(v); term_print("Wallpaper changed."); }
        else term_print("Usage: wall <0-5>");
        return 1;
    }
    if ((a = cmd_arg(c, "accent")) != 0) {
        int v = parse_u(a);
        if (v >= 0 && v < ACCENT_COUNT) { accent_set(v); term_print("Accent changed."); }
        else term_print("Usage: accent <0-5>");
        return 1;
    }
    if ((a = cmd_arg(c, "blur")) != 0) {
        if (streq(a, "off")) g_glass = 0;
        else if (streq(a, "low")) { g_glass = 1; g_blur_lvl = 0; }
        else if (streq(a, "med")) { g_glass = 1; g_blur_lvl = 1; }
        else if (streq(a, "high")) { g_glass = 1; g_blur_lvl = 2; }
        else { term_print("Usage: blur <off|low|med|high>"); return 1; }
        request_redraw(); term_print("Taskbar updated."); return 1;
    }
    return 0;
}

static void term_exec(void) {
    char echo[TERM_COLS];
    int k = 0;
    const char *p = "nexus> ";
    while (p[k]) { echo[k] = p[k]; ++k; }
    for (int i = 0; i < g_term_len && k < TERM_COLS - 1; ++i) echo[k++] = g_term_line[i];
    echo[k] = 0;
    term_print(echo);
    g_term_line[g_term_len] = 0;
    const char *c = g_term_line;
    if (!g_term_len) { }
    else if (streq(c, "help")) {
        term_print("Commands: help clear version uptime date echo <text> exit");
        term_print("Apps: files terminal settings calc notes   ps  kill <app|pid|all|gui>");
        term_print("Power: lock  reboot  shutdown  exit gui (back to command line)");
        term_print("Look: fps [on|off]  hz <N>  wall <0-5>  accent <0-5>  blur <off|low|med|high>");
    } else if (streq(c, "clear")) { g_term_count = 0; }
    else if (streq(c, "version")) term_print("NexusOS " NEXUS_VERSION_DISPLAY);
    else if (streq(c, "uptime")) {
        char b[48]; int n = 0; uint64_t s = pit_get_uptime_seconds();
        const char *l = "Uptime: "; while (l[n]) { b[n] = l[n]; ++n; }
        n += fmt_u(b + n, (uint32_t)(s / 60)); b[n++] = 'm'; b[n++] = ' ';
        n += fmt_u(b + n, (uint32_t)(s % 60)); b[n++] = 's'; b[n] = 0;
        term_print(b);
    } else if (c[0] == 'e' && c[1] == 'c' && c[2] == 'h' && c[3] == 'o' && (c[4] == ' ' || c[4] == 0)) {
        term_print(c[4] ? c + 5 : "");
    } else if (streq(c, "exit") || streq(c, "desktop")) {
        gui_window_close(GUI_WINDOW_TERMINAL);
    } else if (term_extra(c)) {
    } else term_print("Unknown command. Type 'help'.");
    g_term_len = 0; g_term_line[0] = 0;
}

/* ---- Files geometry --------------------------------------------------- */
#define FILES_ROW_H 26
static int files_rows_visible(const gui_window_t *w) {
    int n = (w->height - 32 - 40 - 28 - 24) / FILES_ROW_H;
    return n < 1 ? 1 : n;
}
static int files_list_y(const gui_window_t *w) { return w->y + 32 + 40 + 28; }

static void files_open_selected(void) {
    char names[24][VFS_NAME_LEN]; unsigned char dirs[24];
    int count = vfs_gui_list(names, dirs, 24);
    if (g_files_item == 0) { vfs_cd(".."); g_files_item = 0; g_files_top = 0; }
    else { int i = g_files_item - 1; if (i < count && dirs[i]) { vfs_cd(names[i]); g_files_item = 0; g_files_top = 0; } }
}
static void files_clamp_view(const gui_window_t *w) {
    int rows = files_rows_visible(w);
    if (g_files_item < g_files_top) g_files_top = g_files_item;
    if (g_files_item >= g_files_top + rows) g_files_top = g_files_item - rows + 1;
    if (g_files_top < 0) g_files_top = 0;
}

/* ---- Drawing: windows ------------------------------------------------- */
static void draw_win_buttons(const gui_window_t *w) {
    int base = 100 + (int)w->id * 4;
    int x = w->x + w->width - 46, y = w->y;
    uint32_t fg = 0xE6E7EC;
    /* close */
    if (g_hover == base + 3) rect(x, y, 46, 32, 0xC42B1C);
    for (int i = -5; i <= 5; ++i) { px(x + 23 + i, y + 16 + i, g_hover == base + 3 ? 0xFFFFFF : fg); px(x + 23 + i, y + 16 - i, g_hover == base + 3 ? 0xFFFFFF : fg); }
    /* maximize / restore */
    x -= 46;
    if (g_hover == base + 2) rect(x, y, 46, 32, 0x41444F);
    if (w->maximized) { frame(x + 20, y + 13, 9, 9, fg); frame(x + 17, y + 10, 9, 9, fg); }
    else frame(x + 18, y + 11, 11, 11, fg);
    /* minimize */
    x -= 46;
    if (g_hover == base + 1) rect(x, y, 46, 32, 0x41444F);
    rect(x + 18, y + 16, 11, 1, fg);
}

static void draw_win_files(const gui_window_t *w) {
    char names[24][VFS_NAME_LEN]; unsigned char dirs[24]; char cwd[128];
    int count = vfs_gui_list(names, dirs, 24); vfs_gui_getcwd(cwd, sizeof(cwd));
    int bx = w->x, by = w->y + 32, bw = w->width;
    rounded_rect_alpha(bx + 10, by + 6, bw - 20, 28, 4, C_FIELD, 255);
    text_clip(cwd, bx + 20, by + 12, bw - 40, C_TEXT);
    int hy = by + 40;
    rect(bx, hy + 22, bw, 1, C_WIN_BORDER);
    text_at("Name", bx + 44, hy + 3, 1, C_TEXT_DIM);
    text_at("Type", bx + bw - 130, hy + 3, 1, C_TEXT_DIM);
    files_clamp_view(w);
    int rows = files_rows_visible(w), ly = files_list_y(w);
    int total = count + 1;
    for (int r = 0; r < rows; ++r) {
        int item = g_files_top + r;
        if (item >= total) break;
        int iy = ly + r * FILES_ROW_H;
        if (item == g_files_item) rect(bx + 6, iy, bw - 12, FILES_ROW_H - 2, C_SEL);
        int isdir = item == 0 ? 1 : dirs[item - 1];
        const char *nm = item == 0 ? ".." : names[item - 1];
        if (isdir) icon_folder(bx + 18, iy + 4, 16);
        else { rect(bx + 20, iy + 3, 12, 16, 0xD7DAE3); rect(bx + 20, iy + 3, 12, 3, 0xA9AEBD); }
        text_clip(nm, bx + 44, iy + 5, bw - 190, C_TEXT);
        text_at(isdir ? "Folder" : "File", bx + bw - 130, iy + 5, 1, C_TEXT_DIM);
    }
    int sy = w->y + w->height - 24;
    rect(bx, sy, bw, 24, C_WIN_TITLE_U);
    char b[24]; int n = fmt_u(b, (uint32_t)count); const char *t = " items"; for (int i = 0; t[i]; ++i) b[n++] = t[i]; b[n] = 0;
    text_at(b, bx + 12, sy + 4, 1, C_TEXT_DIM);
    if (!count) text_at("(empty)", bx + 44, ly + FILES_ROW_H + 5, 1, C_TEXT_DIM);
}

static void draw_win_terminal(const gui_window_t *w) {
    int bx = w->x, by = w->y + 32, bw = w->width, bh = w->height - 32;
    rect(bx, by, bw, bh, 0x13141A);
    int lines = (bh - 16 - 28) / 18;
    if (lines < 1) lines = 1;
    int first = g_term_count > lines ? g_term_count - lines : 0;
    for (int i = first; i < g_term_count; ++i)
        text_clip(g_term_lines[i], bx + 12, by + 8 + (i - first) * 18, bw - 24, i < 2 ? C_TEXT_DIM : 0xD9DCE6);
    int py = w->y + w->height - 28;
    rect(bx, py - 4, bw, 1, 0x2A2C35);
    text_at("nexus>", bx + 12, py, 1, 0x58D68D);
    text_clip(g_term_line, bx + 12 + 7 * 8, py, bw - 130, C_TEXT);
    if (w->focused) rect(bx + 12 + (7 + g_term_len) * 8, py + 14, 8, 2, C_ACCENT);
}

/* ---- Settings geometry (shared by drawing and clicking) ------------------- */
static rc_t st_origin(const gui_window_t *w) { return (rc_t){ w->x + 194, w->y + 32 + 22 + 58, 0, 0 }; }
static rc_t st_hz_btn(const gui_window_t *w, int i)   { rc_t o = st_origin(w); return (rc_t){ o.x + i * 58, o.y + 124, 52, 30 }; }
static rc_t st_fps_btn(const gui_window_t *w)         { rc_t o = st_origin(w); return (rc_t){ o.x, o.y + 168, 170, 30 }; }
static rc_t st_wall(const gui_window_t *w, int i)     { rc_t o = st_origin(w); return (rc_t){ o.x + i * 68, o.y + 20, 60, 34 }; }
static rc_t st_glass_btn(const gui_window_t *w, int i){ rc_t o = st_origin(w); return (rc_t){ o.x + i * 92, o.y + 108, 84, 28 }; }
static rc_t st_blur_btn(const gui_window_t *w, int i) { rc_t o = st_origin(w); return (rc_t){ o.x + i * 78, o.y + 166, 70, 28 }; }
static rc_t st_accent(const gui_window_t *w, int i)   { rc_t o = st_origin(w); return (rc_t){ o.x + i * 34, o.y + 224, 24, 24 }; }

static void st_button(rc_t r, const char *label, int selected) {
    rounded_rect_alpha(r.x, r.y, r.w, r.h, 6, selected ? C_ACCENT : C_FIELD, 255);
    text_at(label, r.x + (r.w - text_width(label, 1)) / 2, r.y + (r.h - 16) / 2, 1, selected ? 0x101218 : C_TEXT);
}

static void settings_click(const gui_window_t *w, int mx, int my) {
    if (g_settings_item == 1) {
        for (int i = 0; i < HZ_COUNT; ++i) if (rc_hit(st_hz_btn(w, i), mx, my)) hz_set(i);
        if (rc_hit(st_fps_btn(w), mx, my)) { g_show_fps = !g_show_fps; request_redraw(); }
    } else if (g_settings_item == 2) {
        for (int i = 0; i < WALL_COUNT; ++i) if (rc_hit(st_wall(w, i), mx, my)) wall_select(i);
        for (int i = 0; i < 2; ++i) if (rc_hit(st_glass_btn(w, i), mx, my)) g_glass = i;
        for (int i = 0; i < 3; ++i) if (rc_hit(st_blur_btn(w, i), mx, my)) { g_blur_lvl = i; g_glass = 1; }
        for (int i = 0; i < ACCENT_COUNT; ++i) if (rc_hit(st_accent(w, i), mx, my)) accent_set(i);
    }
}

static void draw_win_settings(const gui_window_t *w) {
    int bx = w->x, by = w->y + 32, bh = w->height - 32;
    rect(bx, by, 170, bh, C_WIN_TITLE_U);
    for (int i = 0; i < SET_CATS; ++i) {
        int iy = by + 12 + i * 40;
        if (i == g_settings_item) { rect(bx + 8, iy, 154, 34, C_SEL); rect(bx + 8, iy + 6, 3, 22, C_ACCENT); }
        text_at(g_set_cats[i], bx + 26, iy + 9, 1, i == g_settings_item ? 0xFFFFFF : C_TEXT);
    }
    int px0 = bx + 194, py0 = by + 22, pw = w->width - 194 - 20;
    text_at(g_set_cats[g_settings_item], px0, py0, 2, C_TEXT);
    rect(px0, py0 + 38, pw, 1, C_WIN_BORDER);
    char b[64]; int n;
    int y = py0 + 58;
#define KV(label, value) do { text_at(label, px0, y, 1, C_TEXT_DIM); text_clip(value, px0 + 130, y, pw - 130, C_TEXT); y += 30; } while (0)
    if (g_settings_item == 0) {
        KV("System", "NexusOS");
        KV("Version", NEXUS_VERSION_DISPLAY);
        KV("Architecture", "x86_64 / UEFI");
        uint64_t s = pit_get_uptime_seconds();
        n = fmt_u(b, (uint32_t)(s / 3600)); b[n++] = 'h'; b[n++] = ' ';
        n += fmt_u(b + n, (uint32_t)((s / 60) % 60)); b[n++] = 'm'; b[n++] = ' ';
        n += fmt_u(b + n, (uint32_t)(s % 60)); b[n++] = 's'; b[n] = 0;
        KV("Uptime", b);
    } else if (g_settings_item == 1) {
        n = fmt_u(b, fb->width); b[n++] = ' '; b[n++] = 'x'; b[n++] = ' '; n += fmt_u(b + n, fb->height); b[n] = 0;
        KV("Resolution", b);
        KV("Color depth", "32-bit");
        if (g_show_fps) { n = fmt_u(b, g_fps); b[n++] = ' '; b[n++] = 'f'; b[n++] = 'p'; b[n++] = 's'; b[n] = 0; KV("Render rate", b); }
        else KV("Render rate", "counter hidden");
        text_at("Frame limit (FPS cap)", px0, y + 14, 1, C_TEXT_DIM);
        for (int i = 0; i < HZ_COUNT; ++i) {
            char t[8]; fmt_u(t, g_hz_opts[i]);
            st_button(st_hz_btn(w, i), t, i == g_hz_idx);
        }
        st_button(st_fps_btn(w), g_show_fps ? "FPS counter: On" : "FPS counter: Off", g_show_fps);
        {   /* Real display refresh (Hz) is separate from the FPS cap above. */
            display_status_t ds;
            display_get_status(&ds);
            if (ds.active_hz) {
                n = 0; const char *l = "Display refresh: "; while (l[n]) { b[n] = l[n]; ++n; }
                n += fmt_u(b + n, ds.active_hz); b[n++] = ' '; b[n++] = 'H'; b[n++] = 'z'; b[n] = 0;
                text_clip(b, px0, y + 116, pw, C_TEXT);
            } else text_clip("Display refresh: unknown (no EDID)", px0, y + 116, pw, C_TEXT_DIM);
            if (ds.below_panel_max) {
                n = 0; const char *l = "Panel supports up to "; while (l[n]) { b[n] = l[n]; ++n; }
                n += fmt_u(b + n, ds.panel_max_hz); const char *l2 = " Hz, firmware drives "; for (int k = 0; l2[k]; ++k) b[n++] = l2[k];
                n += fmt_u(b + n, ds.active_hz); b[n++] = '.'; b[n] = 0;
                text_clip(b, px0, y + 134, pw, C_ACCENT);
            } else if (ds.panel_max_hz) {
                n = 0; const char *l = "Panel maximum: "; while (l[n]) { b[n] = l[n]; ++n; }
                n += fmt_u(b + n, ds.panel_max_hz); b[n++] = ' '; b[n++] = 'H'; b[n++] = 'z'; b[n] = 0;
                text_clip(b, px0, y + 134, pw, C_TEXT_DIM);
            }
            if (ds.gpu_desc[0]) {
                n = 0; const char *l = "GPU: "; while (l[n]) { b[n] = l[n]; ++n; }
                for (int k = 0; ds.gpu_desc[k] && n < 60; ++k) b[n++] = ds.gpu_desc[k];
                b[n] = 0;
                text_clip(b, px0, y + 152, pw, C_TEXT_DIM);
            }
            text_clip(ds.modeset_available ? "Hz switching: available"
                                           : "Hz switching: unavailable (no modeset driver)",
                      px0, y + 170, pw, C_TEXT_DIM);
            text_clip("FPS cap does not change monitor Hz.", px0, y + 188, pw, C_TEXT_DIM);
        }
    } else if (g_settings_item == 2) {
        rc_t o = st_origin(w);
        text_at("Wallpaper", o.x, o.y, 1, C_TEXT_DIM);
        for (int i = 0; i < WALL_COUNT; ++i) {
            rc_t r = st_wall(w, i);
            draw_wall_thumb(r.x, r.y, r.w, r.h, i);
            if (i == g_wall_idx) { frame(r.x - 2, r.y - 2, r.w + 4, r.h + 4, C_ACCENT); frame(r.x - 3, r.y - 3, r.w + 6, r.h + 6, C_ACCENT); }
            else frame(r.x - 1, r.y - 1, r.w + 2, r.h + 2, C_WIN_BORDER);
        }
        {
            const uint8_t *tmp; int tw = 0, th = 0;
            char cap[64]; int k = 0; const char *nm = g_wall_idx == 0 ? "Default" : "Custom ";
            while (nm[k]) { cap[k] = nm[k]; ++k; }
            if (g_wall_idx > 0) {
                cap[k++] = (char)('0' + g_wall_idx);
                if (wall_get(g_wall_idx, &tmp, &tw, &th) && tw < 100) {
                    const char *ph = " (placeholder: add wall";
                    for (int i = 0; ph[i]; ++i) cap[k++] = ph[i];
                    cap[k++] = (char)('0' + g_wall_idx);
                    const char *ext = ".png)";
                    for (int i = 0; ext[i]; ++i) cap[k++] = ext[i];
                }
            }
            cap[k] = 0;
            text_clip(cap, o.x, o.y + 62, pw, C_TEXT);
        }
        text_at("Taskbar", o.x, o.y + 90, 1, C_TEXT_DIM);
        st_button(st_glass_btn(w, 0), "Solid", !g_glass);
        st_button(st_glass_btn(w, 1), "Glass", g_glass);
        text_at("Blur strength", o.x, o.y + 148, 1, C_TEXT_DIM);
        st_button(st_blur_btn(w, 0), "Low", g_blur_lvl == 0);
        st_button(st_blur_btn(w, 1), "Medium", g_blur_lvl == 1);
        st_button(st_blur_btn(w, 2), "High", g_blur_lvl == 2);
        text_at("Accent color", o.x, o.y + 206, 1, C_TEXT_DIM);
        for (int i = 0; i < ACCENT_COUNT; ++i) {
            rc_t r = st_accent(w, i);
            rounded_rect_alpha(r.x, r.y, r.w, r.h, 6, g_accents[i], 255);
            if (i == g_accent_idx) frame(r.x - 3, r.y - 3, r.w + 6, r.h + 6, 0xFFFFFF);
        }
    } else if (g_settings_item == 3) {
        KV("Pointer", mouse_is_present() ? "Detected" : "Not detected");
        KV("Scroll wheel", mouse_has_wheel() ? "Yes" : "No");
        KV("Keyboard", "PS/2 or USB HID");
    } else {
        KV("NexusOS", NEXUS_VERSION_DISPLAY);
        KV("Desktop", "NexusOS Desktop 3.0");
        KV("Author", "julfy-0");
    }
#undef KV
}

static void draw_win_calc(const gui_window_t *w) {
    int bx = w->x, by = w->y + 32;
    rounded_rect_alpha(bx + 12, by + 12, w->width - 24, 64, 8, 0x14151A, 255);
    char d[24]; calc_display(d);
    const char *opn = g_calc_op == '/' ? "/" : g_calc_op == '*' ? "x" : g_calc_op == '-' ? "-" : g_calc_op == '+' ? "+" : "";
    if (opn[0]) text_at(opn, bx + 24, by + 20, 1, C_ACCENT);
    int len = slen(d), maxc = (w->width - 24 - 24) / 16;
    const char *show = d;
    if (maxc < 1) maxc = 1;
    if (len > maxc) { show = d + (len - maxc); len = maxc; }
    text_at(show, bx + w->width - 24 - len * 16, by + 12 + 16, 2, g_calc_err ? 0xFF6B6B : 0xFFFFFF);
    for (int r = 0; r < 5; ++r) for (int c = 0; c < 4; ++c) {
        rc_t bt = calc_btn(w, r, c);
        uint32_t col = C_FIELD;
        int dark = 0;
        if (g_calc_code[r][c] == '=') { col = C_ACCENT; dark = 1; }
        else if (c == 3) col = 0x43465A;
        else if (r == 0) col = 0x3A3D47;
        rounded_rect_alpha(bt.x, bt.y, bt.w, bt.h, 8, col, 255);
        const char *l = g_calc_lbl[r][c];
        text_at(l, bt.x + (bt.w - text_width(l, 1)) / 2, bt.y + (bt.h - 16) / 2, 1, dark ? 0x101218 : C_TEXT);
    }
}

static void draw_win_notes(const gui_window_t *w) {
    int bx = w->x, by = w->y + 32;
    int fh = w->height - 32 - 12 - 52;
    rounded_rect_alpha(bx + 12, by + 12, w->width - 24, fh, 8, 0x14151A, 255);
    int cols = (w->width - 24 - 16) / 8, vis = (fh - 12) / 18;
    if (cols < 1) cols = 1;
    if (vis < 1) vis = 1;
    int total = 1, c = 0;
    for (int i = 0; i < g_note_len; ++i) {
        if (g_note[i] == '\n') { total++; c = 0; }
        else { if (c == cols) { total++; c = 0; } c++; }
    }
    if (c == cols) total++;
    int skip = total > vis ? total - vis : 0;
    int row = 0; c = 0;
    for (int i = 0; i < g_note_len; ++i) {
        char ch = g_note[i];
        if (ch == '\n') { row++; c = 0; continue; }
        if (c == cols) { row++; c = 0; }
        if (row >= skip && row - skip < vis) glyph(bx + 20 + c * 8, by + 18 + (row - skip) * 18, ch, 1, C_TEXT);
        c++;
    }
    if (c == cols) { row++; c = 0; }
    if (w->focused && row >= skip && row - skip < vis) rect(bx + 20 + c * 8, by + 18 + (row - skip) * 18 + 14, 8, 2, C_ACCENT);
    if (!g_note_len) text_at("Start typing...", bx + 20, by + 18, 1, C_TEXT_DIM);
    rc_t sb = note_save_btn(w), cb = note_clear_btn(w);
    st_button(sb, "Save", 1);
    st_button(cb, "Clear", 0);
    char cnt[24]; int n = fmt_u(cnt, (uint32_t)g_note_len); cnt[n++] = '/'; n += fmt_u(cnt + n, NOTE_CAP); cnt[n] = 0;
    text_at(cnt, w->x + w->width - 12 - n * 8, sb.y + 6, 1, C_TEXT_DIM);
}

/* ---- Window chrome: rounded corners, soft shadow, glass ------------------
 * Windows are glass: the area behind a window is blurred (WIN_BLUR_RADIUS),
 * then the window background is laid over it with WIN_BG_ALPHA. Corners are
 * anti-aliased: the corner squares are saved before the window is drawn and
 * blended back through a coverage mask afterwards, so whatever the app draws
 * (opaque panels, status bars, hover highlights) can never poke out of the
 * rounded corners. The shadow is a smooth falloff drawn only OUTSIDE the
 * window, so it never shows through the translucent glass. */
#define WIN_RADIUS              8      /* corner radius in px (Windows 11 look) */
#define WIN_SHADOW_SPREAD       28     /* how far the soft shadow reaches, px */
#define WIN_SHADOW_DY           8      /* shadow is shifted down a little, px */
#define WIN_SHADOW_MAX          120    /* shadow darkness (0-255) */
#define WIN_BG_TRANSPARENCY_PCT 75     /* window background transparency, % */
#define WIN_BG_ALPHA            ((uint8_t)((255 * (100 - WIN_BG_TRANSPARENCY_PCT)) / 100))
#define WIN_BLUR_RADIUS         256    /* blur radius behind windows, px */

static uint8_t  g_win_cov[WIN_RADIUS][WIN_RADIUS];    /* corner coverage 0..255 */
static uint8_t  g_win_ring[WIN_RADIUS][WIN_RADIUS];   /* 1px outline coverage 0..255 */
static uint8_t  g_win_inset[WIN_RADIUS];              /* per corner row: px left of full coverage */
static uint8_t  g_shadow_prof[2 * WIN_SHADOW_SPREAD + 1];
static uint32_t g_win_save[4][WIN_RADIUS * WIN_RADIUS];
static int      g_win_tables_ready;

static uint32_t win_isqrt(uint32_t n) {
    uint32_t r = 0, bit = 1u << 30;
    while (bit > n) bit >>= 2;
    while (bit) {
        if (n >= r + bit) { n -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    return r;
}

static void win_tables_init(void) {
    if (g_win_tables_ready) return;
    g_win_tables_ready = 1;
    for (int yy = 0; yy < WIN_RADIUS; ++yy) {
        g_win_inset[yy] = WIN_RADIUS;
        for (int xx = 0; xx < WIN_RADIUS; ++xx) {
            /* distance of the pixel centre from the arc centre, in 1/16 px */
            int hx = 2 * (WIN_RADIUS - xx) - 1, hy = 2 * (WIN_RADIUS - yy) - 1;
            int d16 = (int)win_isqrt((uint32_t)(hx * hx + hy * hy) * 64u);
            int outer = WIN_RADIUS * 16 - d16 + 8;
            int inner = (WIN_RADIUS - 1) * 16 - d16 + 8;
            if (outer < 0) outer = 0;
            if (outer > 16) outer = 16;
            if (inner < 0) inner = 0;
            if (inner > 16) inner = 16;
            g_win_cov[yy][xx] = (uint8_t)(outer * 255 / 16);
            g_win_ring[yy][xx] = (uint8_t)((outer - inner) * 255 / 16);
            if (outer == 16 && g_win_inset[yy] == WIN_RADIUS) g_win_inset[yy] = (uint8_t)xx;
        }
    }
    /* smoothstep falloff across [-SPREAD, +SPREAD] around the shadow edge */
    for (int i = 0; i <= 2 * WIN_SHADOW_SPREAD; ++i) {
        int t = i * 1024 / (2 * WIN_SHADOW_SPREAD);
        int s = ((t * t) >> 10) * (3072 - 2 * t) >> 10;
        g_shadow_prof[i] = (uint8_t)(s * 255 / 1024);
    }
}

static inline uint32_t *win_px_ptr(int x, int y) {
    if (!fb || x < 0 || y < 0 || (uint32_t)x >= fb->width || (uint32_t)y >= fb->height) return 0;
    return fb_row((uint32_t)y) + x;
}

/* Corner c: bit0 = right, bit1 = bottom. */
static void win_corner_origin(const gui_window_t *w, int c, int *ox, int *oy) {
    *ox = (c & 1) ? w->x + w->width - WIN_RADIUS : w->x;
    *oy = (c & 2) ? w->y + w->height - WIN_RADIUS : w->y;
}

static inline int win_corner_idx(int c, int sx, int sy, int *xx, int *yy) {
    *xx = (c & 1) ? WIN_RADIUS - 1 - sx : sx;
    *yy = (c & 2) ? WIN_RADIUS - 1 - sy : sy;
    return 0;
}

/* Soft drop shadow, drawn only outside the window shape. */
static void win_shadow(const gui_window_t *w) {
    const int S = WIN_SHADOW_SPREAD;
    int bx0 = w->x, bx1 = w->x + w->width;
    int by0 = w->y + WIN_SHADOW_DY, by1 = w->y + w->height + WIN_SHADOW_DY;
    int x0 = bx0 - S, x1 = bx1 + S, y0 = by0 - S, y1 = by1 + S;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int)fb->width) x1 = (int)fb->width;
    if (y1 > (int)fb->height) y1 = (int)fb->height;
    for (int y = y0; y < y1; ++y) {
        int dy = y - by0, dy2 = by1 - 1 - y;
        int iy = S + (dy < dy2 ? dy : dy2);
        if (iy < 0) continue;
        if (iy > 2 * S) iy = 2 * S;
        uint32_t py = g_shadow_prof[iy];
        if (!py) continue;
        /* part of this row that belongs to the window itself (no shadow there) */
        int hx0 = 0, hx1 = 0, ry = y - w->y;
        if (ry >= 0 && ry < w->height) {
            int inset = 0;
            if (ry < WIN_RADIUS) inset = g_win_inset[ry];
            else if (ry >= w->height - WIN_RADIUS) inset = g_win_inset[w->height - 1 - ry];
            hx0 = w->x + inset;
            hx1 = w->x + w->width - inset;
        }
        uint32_t *row = fb_row((uint32_t)y);
        uint32_t black = pack(0x000000);
        for (int x = x0; x < x1; ++x) {
            if (x >= hx0 && x < hx1) { x = hx1 - 1; continue; }
            int dx = x - bx0, dx2 = bx1 - 1 - x;
            int ix = S + (dx < dx2 ? dx : dx2);
            if (ix < 0) continue;
            if (ix > 2 * S) ix = 2 * S;
            uint32_t a = ((uint32_t)g_shadow_prof[ix] * py) >> 8;     /* 0..254 */
            a = (a * WIN_SHADOW_MAX) >> 8;
            if (a) row[x] = blend_word(row[x], black, (uint8_t)a);
        }
    }
}

static void win_corners_save(const gui_window_t *w) {
    for (int c = 0; c < 4; ++c) {
        int ox, oy;
        win_corner_origin(w, c, &ox, &oy);
        for (int sy = 0; sy < WIN_RADIUS; ++sy) for (int sx = 0; sx < WIN_RADIUS; ++sx) {
            uint32_t *p = win_px_ptr(ox + sx, oy + sy);
            g_win_save[c][sy * WIN_RADIUS + sx] = p ? *p : 0;
        }
    }
}

/* Blend the saved background back in outside the rounded shape. */
static void win_corners_mask(const gui_window_t *w) {
    for (int c = 0; c < 4; ++c) {
        int ox, oy;
        win_corner_origin(w, c, &ox, &oy);
        for (int sy = 0; sy < WIN_RADIUS; ++sy) for (int sx = 0; sx < WIN_RADIUS; ++sx) {
            int xx, yy;
            win_corner_idx(c, sx, sy, &xx, &yy);
            uint8_t cov = g_win_cov[yy][xx];
            if (cov == 255) continue;
            uint32_t *p = win_px_ptr(ox + sx, oy + sy);
            if (!p) continue;
            uint32_t saved = g_win_save[c][sy * WIN_RADIUS + sx];
            *p = cov ? blend_word(saved, *p, cov) : saved;
        }
    }
}

/* 1px outline following the rounded shape (anti-aliased arcs). */
static void win_border(const gui_window_t *w, uint32_t color, int skip_top) {
    int x = w->x, y = w->y, W = w->width, H = w->height, R = WIN_RADIUS;
    if (!skip_top) rect(x + R, y, W - 2 * R, 1, color);
    rect(x + R, y + H - 1, W - 2 * R, 1, color);
    rect(x, y + R, 1, H - 2 * R, color);
    rect(x + W - 1, y + R, 1, H - 2 * R, color);
    for (int c = 0; c < 4; ++c) {
        int ox, oy;
        win_corner_origin(w, c, &ox, &oy);
        for (int sy = 0; sy < WIN_RADIUS; ++sy) for (int sx = 0; sx < WIN_RADIUS; ++sx) {
            int xx, yy;
            win_corner_idx(c, sx, sy, &xx, &yy);
            uint8_t ring = g_win_ring[yy][xx];
            if (ring) blend_px(ox + sx, oy + sy, color, ring);
        }
    }
}

static void draw_window(const gui_window_t *w) {
    int rounded = !w->maximized;
    uint8_t bg_a = g_glass ? WIN_BG_ALPHA : 255;
    if (rounded) { win_tables_init(); win_shadow(w); win_corners_save(w); }
    if (g_glass) blur_rect(w->x, w->y, w->width, w->height, WIN_BLUR_RADIUS);
    rect_alpha(w->x, w->y + 32, w->width, w->height - 32, C_WIN_BODY, bg_a);
    rect_alpha(w->x, w->y, w->width, 32, w->focused ? C_WIN_TITLE_F : C_WIN_TITLE_U, bg_a);
    if (w->focused) rect(w->x, w->y, w->width, 2, C_ACCENT);
    int idx = app_of_id(w->id);
    app_icon(idx, w->x + 10, w->y + 8, 16, w->focused ? C_WIN_TITLE_F : C_WIN_TITLE_U);
    if (w->title) text_clip(w->title, w->x + 34, w->y + 8, w->width - 34 - 150, w->focused ? 0xFFFFFF : C_TEXT_DIM);
    draw_win_buttons(w);
    if (w->id == GUI_WINDOW_FILES) draw_win_files(w);
    else if (w->id == GUI_WINDOW_TERMINAL) draw_win_terminal(w);
    else if (w->id == GUI_WINDOW_SETTINGS) draw_win_settings(w);
    else if (w->id == GUI_WINDOW_CALC) draw_win_calc(w);
    else if (w->id == GUI_WINDOW_NOTES) draw_win_notes(w);
    if (rounded) { win_corners_mask(w); win_border(w, w->focused ? 0x4A4E5C : C_WIN_BORDER, w->focused); }
}

/* ---- Drawing: taskbar, menu -------------------------------------------- */
/* Hover / active highlight that works on both the solid and the glass taskbar. */
static void tb_hi(rc_t r, int active) {
    if (g_glass) rect_alpha(r.x, r.y, r.w, r.h, 0xFFFFFF, active ? 42 : 26);
    else rect(r.x, r.y, r.w, r.h, active ? C_TB_ACTIVE : C_TB_HOVER);
}

static void draw_taskbar(void) {
    int top = tb_top(), h = tb_h(), W = (int)fb->width;
    if (g_glass) {
        blur_rect(0, top, W, h, g_blur_r[g_blur_lvl]);
        rect_alpha(0, top, W, h, C_TASKBAR, 170);
    } else rect(0, top, W, h, C_TASKBAR);
    rect(0, top, W, 1, 0x3C3F49);

    /* Start button */
    rc_t s = r_start();
    if (g_menu_open) tb_hi(s, 1); else if (g_hover == 1) tb_hi(s, 0);
    icon_nexus(s.x + s.w / 2, s.y + s.h / 2, C_ACCENT);

    /* Search box (hidden on narrow screens) */
    rc_t q = r_search();
    if (q.w) {
        rounded_rect_alpha(q.x, q.y, q.w, q.h, 6, C_FIELD, gl(g_hover == 2 ? 230 : 190));
        icon_search(q.x + 18, q.y + q.h / 2, C_TEXT_DIM);
        text_at("Search", q.x + 36, q.y + (q.h - 16) / 2, 1, C_TEXT_DIM);
    }

    /* Window buttons */
    for (int i = 0; i < tb_count; ++i) {
        gui_window_t *w = gui_window_get((gui_window_id_t)tb_order[i]);
        if (!w) continue;
        rc_t r = r_task(i);
        int active = !w->minimized && gui_window_active_id() == (int)w->id;
        if (active) tb_hi(r, 1); else if (g_hover == 10 + i) tb_hi(r, 0);
        app_icon(app_of_id(w->id), r.x + 8, r.y + (r.h - 16) / 2, 16, active ? C_TB_ACTIVE : C_TASKBAR);
        if (w->title && r.w > 60) text_clip(w->title, r.x + 32, r.y + (r.h - 16) / 2, r.w - 36, w->minimized ? C_TEXT_DIM : C_TEXT);
        rect(r.x + 6, r.y + r.h - 3, r.w - 12, 2, active ? C_ACCENT : (w->minimized ? C_TEXT_DIM : C_WIN_BORDER));
    }

    /* Tray + clock */
    rc_t t = r_tray(), c = r_clock();
    if (g_hover == 4) tb_hi(t, 0);
    icon_net(t.x + 10, t.y + (t.h - 14) / 2, C_TEXT);
    icon_speaker(t.x + 32, t.y + (t.h - 14) / 2 - 1, C_TEXT);
    if (g_hover == 5) tb_hi(c, 0);
    char tm[8];
    fmt_2d(tm, g_clock_h); tm[2] = ':'; fmt_2d(tm + 3, g_clock_m);
    text_at(tm, c.x + c.w - 8 - text_width(tm, 1), c.y + c.h / 2 - 16, 1, C_TEXT);
    char d[16]; int n = 0;
    n += fmt_2d(d + n, g_date_d); d[n++] = '.'; n += fmt_2d(d + n, g_date_mo); d[n++] = '.'; n += fmt_u(d + n, (uint32_t)g_date_y); d[n] = 0;
    text_at(d, c.x + c.w - 8 - text_width(d, 1), c.y + c.h / 2, 1, C_TEXT_DIM);

    /* Show-desktop sliver */
    rc_t sd = r_desktop_btn();
    if (g_hover == 3) rect_alpha(sd.x, sd.y, sd.w, sd.h, 0xFFFFFF, 40);
    rect(sd.x, sd.y + 6, 1, sd.h - 12, 0x4A4E5C);
}


static void draw_start_menu(void) {
    rc_t m = r_menu();
    rect_alpha(m.x + 4, m.y + 6, m.w, m.h, 0x000000, 100);
    if (g_glass) {
        blur_rect(m.x, m.y, m.w, m.h, g_blur_r[g_blur_lvl]);
        rect_alpha(m.x, m.y, m.w, m.h, C_MENU, 195);
    } else rect(m.x, m.y, m.w, m.h, C_MENU);
    frame(m.x, m.y, m.w, m.h, 0x41444F);

    rc_t q = r_menu_search();
    rounded_rect_alpha(q.x, q.y, q.w, q.h, 6, C_FIELD, gl(190));
    icon_search(q.x + 20, q.y + q.h / 2, C_TEXT_DIM);
    if (g_search_len) text_clip(g_search_query, q.x + 40, q.y + 10, q.w - 56, C_TEXT);
    else text_at("Type to search apps", q.x + 40, q.y + 10, 1, C_TEXT_DIM);
    rect(q.x + 40 + g_search_len * 8, q.y + 28, 8, 2, C_ACCENT);

    text_at("Apps", m.x + 20, m.y + 68, 1, C_TEXT_DIM);
    int list[NAPPS]; int n = menu_list(list);
    for (int i = 0; i < n; ++i) {
        rc_t r = r_menu_row(i);
        int sel = (i == g_menu_sel), hov = (g_hover == 200 + i);
        if (sel || hov) rect(r.x, r.y, r.w, r.h, sel ? C_SEL : C_TB_HOVER);
        app_icon(list[i], r.x + 12, r.y + 9, 32, sel ? C_SEL : (hov ? C_TB_HOVER : C_MENU));
        text_at(g_apps[list[i]].name, r.x + 60, r.y + 17, 1, C_TEXT);
        if (gui_window_get(g_apps[list[i]].id)) text_at("running", r.x + r.w - 76, r.y + 17, 1, C_TEXT_DIM);
    }
    if (!n) text_at("No results", m.x + 24, m.y + 100, 1, C_TEXT_DIM);

    int by = m.y + m.h - 60;
    rect_alpha(m.x, by, m.w, 60, C_WIN_TITLE_U, gl(150));
    rect(m.x, by, m.w, 1, 0x3C3F49);
    circle(m.x + 36, by + 30, 14, C_ACCENT, 255);
    text_at("N", m.x + 32, by + 22, 1, 0xFFFFFF);
    text_at("NexusOS", m.x + 60, by + 22, 1, C_TEXT);
    rc_t rb = r_btn_reboot(), sb = r_btn_shutdown(), lb = r_btn_lock();
    rounded_rect_alpha(lb.x, lb.y, lb.w, lb.h, 6, g_hover == 249 ? 0x41444F : C_FIELD, 255);
    text_at("Lock", lb.x + (lb.w - 32) / 2, lb.y + 10, 1, C_TEXT);
    rounded_rect_alpha(rb.x, rb.y, rb.w, rb.h, 6, g_hover == 250 ? 0x41444F : C_FIELD, 255);
    text_at("Restart", rb.x + (rb.w - 56) / 2, rb.y + 10, 1, C_TEXT);
    rounded_rect_alpha(sb.x, sb.y, sb.w, sb.h, 6, g_hover == 251 ? 0xD94A3A : 0xA83A2E, 255);
    text_at("Shut down", sb.x + (sb.w - 72) / 2, sb.y + 10, 1, 0xFFFFFF);
}

static void draw_message(void) {
    if (!g_gui_message || !g_gui_message[0]) return;
    int w = text_width(g_gui_message, 1) + 28, h = 40;
    int x = (int)fb->width - w - 16, y = tb_top() - h - 12;
    rect_alpha(x + 3, y + 4, w, h, 0x000000, 90);
    rect(x, y, w, h, C_MENU);
    frame(x, y, w, h, 0x4A4E5C);
    rect(x, y, 4, h, C_ACCENT);
    text_at(g_gui_message, x + 16, y + 12, 1, C_TEXT);
}

static void draw_fps_overlay(void) {
    if (!g_show_fps) return;
    char b[24]; int n = fmt_u(b, g_fps);
    b[n++] = ' '; b[n++] = 'F'; b[n++] = 'P'; b[n++] = 'S'; b[n] = 0;
    int w = text_width(b, 1) + 20, x = (int)fb->width - w - 8;
    rect_alpha(x, 8, w, 28, 0x000000, 150);
    text_at(b, x + 10, 14, 1, 0x9FE6B8);
}

static void draw_lockscreen(void) {
    draw_wallpaper();
    rect_alpha(0, 0, (int)fb->width, (int)fb->height, 0x000000, 110);
    char t[8];
    fmt_2d(t, g_clock_h); t[2] = ':'; fmt_2d(t + 3, g_clock_m);
    int sc = (int)fb->width >= 1100 ? 10 : ((int)fb->width >= 700 ? 6 : 3);
    int ty = (int)fb->height / 3 - 8 * sc;
    text_at(t, ((int)fb->width - text_width(t, sc)) / 2, ty, sc, 0xFFFFFF);
    char d[16]; int n = 0;
    n += fmt_2d(d + n, g_date_d); d[n++] = '.'; n += fmt_2d(d + n, g_date_mo); d[n++] = '.'; n += fmt_u(d + n, (uint32_t)g_date_y); d[n] = 0;
    text_center(d, ty + 16 * sc + 20, 2, 0xE6E8EF);
    if (g_lock_passwd_enabled) {
        /* Password input box */
        int bw = 220, bh = 36;
        int bx = ((int)fb->width - bw) / 2;
        int by = ty + 16 * sc + 60;
        rounded_rect_alpha(bx, by, bw, bh, 8, 0x1E2030, 220);
        /* Hint or masked input */
        if (g_lock_input_len == 0) {
            int hw = text_width("Enter PIN to unlock", 1);
            text_at("Enter PIN to unlock", bx + (bw - hw) / 2, by + (bh - 16) / 2, 1, 0x7A7E8E);
        } else {
            char masked[32]; int mi = 0;
            for (int i = 0; i < g_lock_input_len && mi < 31; i++) masked[mi++] = '*';
            masked[mi] = 0;
            int mw = text_width(masked, 1);
            text_at(masked, bx + (bw - mw) / 2, by + (bh - 16) / 2, 1, 0xFFFFFF);
        }
        /* Error message */
        if (g_lock_error && g_lock_error[0]) {
            int ew = text_width(g_lock_error, 1);
            text_at(g_lock_error, ((int)fb->width - ew) / 2, by + bh + 10, 1, 0xFF5555);
        }
    } else {
        text_center("Click or press any key to unlock", (int)fb->height - 80, 1, 0xC9CCD6);
    }
}

static void draw_scene(void) {
    tb_sync();
    if (g_locked && !g_unlock_anim_active) { draw_lockscreen(); return; }
    if (g_unlock_anim_active) {
        /* Draw desktop underneath */
        draw_wallpaper();
        draw_desktop_icons();
        gui_window_id_t st[GUI_WINDOW_MAX];
        int n = gui_window_stack(st);
        for (int i = 0; i < n; ++i) { gui_window_t *w = gui_window_get(st[i]); if (w) draw_window(w); }
        draw_taskbar();
        /* Overlay the lock screen fading out */
        uint8_t overlay_alpha = (uint8_t)(110 * (20 - g_unlock_anim) / 20);
        if (overlay_alpha > 0)
            rect_alpha(0, 0, (int)fb->width, (int)fb->height, 0x000000, overlay_alpha);
        return;
    }
    draw_wallpaper();
    draw_desktop_icons();
    gui_window_id_t st[GUI_WINDOW_MAX];
    int n = gui_window_stack(st);
    for (int i = 0; i < n; ++i) { gui_window_t *w = gui_window_get(st[i]); if (w) draw_window(w); }
    draw_taskbar();
    if (g_menu_open) draw_start_menu();
    draw_message();
    draw_fps_overlay();
}

/* ---- Hit testing ------------------------------------------------------- */
static int compute_hover(int mx, int my) {
    if (g_menu_open) {
        rc_t m = r_menu();
        if (rc_hit(m, mx, my)) {
            int list[NAPPS]; int n = menu_list(list);
            for (int i = 0; i < n; ++i) if (rc_hit(r_menu_row(i), mx, my)) return 200 + i;
            if (rc_hit(r_btn_lock(), mx, my)) return 249;
            if (rc_hit(r_btn_reboot(), mx, my)) return 250;
            if (rc_hit(r_btn_shutdown(), mx, my)) return 251;
            return 0;
        }
    }
    if (my >= tb_top()) {
        if (rc_hit(r_start(), mx, my)) return 1;
        rc_t q = r_search(); if (q.w && rc_hit(q, mx, my)) return 2;
        if (rc_hit(r_desktop_btn(), mx, my)) return 3;
        if (rc_hit(r_tray(), mx, my)) return 4;
        if (rc_hit(r_clock(), mx, my)) return 5;
        for (int i = 0; i < tb_count; ++i) if (rc_hit(r_task(i), mx, my)) return 10 + i;
        return 0;
    }
    gui_window_id_t id = gui_window_at(mx, my);
    if (id) {
        gui_window_t *w = gui_window_get(id);
        int b = w ? win_btn_hit(w, mx, my) : 0;
        if (b) return 100 + (int)id * 4 + (b == 3 ? 3 : b == 2 ? 2 : 1);
        return 0;
    }
    for (int i = 0; i < NAPPS; ++i) if (rc_hit(r_icon(i), mx, my)) return 300 + i;
    return 0;
}

static int is_double(int tag) {
    uint64_t now = pit_get_uptime_ms();
    int d = (tag == g_last_tag && now - g_last_click_ms < 450);
    g_last_tag = d ? -1 : tag;
    g_last_click_ms = now;
    return d;
}

static void menu_open(void) { g_menu_open = 1; g_menu_sel = 0; g_search_len = 0; g_search_query[0] = 0; }

static int ui_click(int mx, int my) {
    tb_sync();
    if (g_menu_open) {
        rc_t m = r_menu();
        if (rc_hit(m, mx, my)) {
            int list[NAPPS]; int n = menu_list(list);
            for (int i = 0; i < n; ++i) if (rc_hit(r_menu_row(i), mx, my)) { launch_app(list[i]); g_menu_open = 0; return 1; }
            if (rc_hit(r_btn_lock(), mx, my)) { g_menu_open = 0; g_locked = 1; return 1; }
            if (rc_hit(r_btn_reboot(), mx, my)) { power_action(1); return 1; }
            if (rc_hit(r_btn_shutdown(), mx, my)) { power_action(0); return 1; }
            return 1;
        }
        g_menu_open = 0;
        if (rc_hit(r_start(), mx, my)) return 1;
        /* otherwise the click continues to whatever is underneath */
    }

    if (my >= tb_top()) {
        if (rc_hit(r_start(), mx, my)) { menu_open(); return 1; }
        rc_t q = r_search(); if (q.w && rc_hit(q, mx, my)) { menu_open(); return 1; }
        if (rc_hit(r_desktop_btn(), mx, my)) { gui_window_minimize_all(); return 1; }
        for (int i = 0; i < tb_count; ++i) if (rc_hit(r_task(i), mx, my)) {
            gui_window_t *w = gui_window_get((gui_window_id_t)tb_order[i]);
            if (!w) return 1;
            if (w->minimized || gui_window_active_id() != (int)w->id) gui_window_focus(w->id);
            else gui_window_minimize(w->id);
            return 1;
        }
        return 1;
    }

    gui_window_id_t id = gui_window_at(mx, my);
    if (id) {
        gui_window_focus(id);
        g_icon_sel = -1;
        gui_window_t *w = gui_window_get(id);
        if (!w) return 1;
        if (my < w->y + 32) {
            int b = win_btn_hit(w, mx, my);
            if (b == 3) { gui_window_close(id); return 1; }
            if (b == 2) { gui_window_toggle_maximize(id); return 1; }
            if (b == 1) { gui_window_minimize(id); return 1; }
            if (is_double(3000 + (int)id)) { gui_window_toggle_maximize(id); return 1; }
            g_drag_id = (int)id; g_drag_dx = mx - w->x; g_drag_dy = my - w->y;
            return 1;
        }
        if (id == GUI_WINDOW_FILES) {
            int ly = files_list_y(w);
            if (my >= ly && mx >= w->x + 6 && mx < w->x + w->width - 6) {
                int row = (my - ly) / FILES_ROW_H, item = g_files_top + row;
                char names[24][VFS_NAME_LEN]; unsigned char dirs[24];
                int count = vfs_gui_list(names, dirs, 24);
                if (row < files_rows_visible(w) && item <= count) {
                    g_files_item = item;
                    if (is_double(2000 + item)) files_open_selected();
                }
            }
        } else if (id == GUI_WINDOW_SETTINGS) {
            if (mx < w->x + 170 && my >= w->y + 32 + 12) {
                int i = (my - (w->y + 32 + 12)) / 40;
                if (i >= 0 && i < SET_CATS) g_settings_item = i;
            } else if (mx >= w->x + 170) settings_click(w, mx, my);
        } else if (id == GUI_WINDOW_CALC) {
            for (int r = 0; r < 5; ++r) for (int c = 0; c < 4; ++c)
                if (rc_hit(calc_btn(w, r, c), mx, my)) calc_press(g_calc_code[r][c]);
        } else if (id == GUI_WINDOW_NOTES) {
            if (rc_hit(note_save_btn(w), mx, my)) notes_save();
            else if (rc_hit(note_clear_btn(w), mx, my)) { g_note_len = 0; g_note[0] = 0; }
        }
        return 1;
    }

    for (int i = 0; i < NAPPS; ++i) if (rc_hit(r_icon(i), mx, my)) {
        g_icon_sel = i;
        if (is_double(1000 + i)) launch_app(i);
        return 1;
    }
    g_icon_sel = -1;
    return 1;
}

static int ui_wheel(int wheel) {
    int aid = gui_window_active_id();
    if (aid == GUI_WINDOW_FILES) {
        char names[24][VFS_NAME_LEN]; unsigned char dirs[24];
        int total = vfs_gui_list(names, dirs, 24) + 1;
        if (wheel > 0 && g_files_item > 0) g_files_item--;
        else if (wheel < 0 && g_files_item + 1 < total) g_files_item++;
        return 1;
    }
    if (aid == GUI_WINDOW_SETTINGS) {
        if (wheel > 0) g_settings_item = (g_settings_item + SET_CATS - 1) % SET_CATS;
        else g_settings_item = (g_settings_item + 1) % SET_CATS;
        return 1;
    }
    return 0;
}

/* ---- Public API ---------------------------------------------------------- */
void gui_init(nexus_framebuffer_t *f) {
    fb = f;
    g_cursor_visible = 0;
    g_backbuffer = NULL;
    g_backbuffer_bytes = 0;
    g_render_stride = 0;
    if (f) {
        g_fb_base = (uint64_t)f->base;
        g_fb_stride = f->pixels_per_scanline * 4u;
        g_fb_bgr = (f->pixel_format == NEXUS_PIXFMT_BGR);
        g_render_stride = f->width * 4u;
        g_backbuffer_bytes = (uint64_t)f->width * (uint64_t)f->height * 4u;
        if (g_backbuffer_bytes <= (256ULL << 20))
            g_backbuffer = (uint32_t *)kmalloc((size_t)g_backbuffer_bytes);
        if (g_backbuffer) memset(g_backbuffer, 0, (size_t)g_backbuffer_bytes);
        mouse_set_screen_size(f->width, f->height);
        gui_window_manager_init((int)f->width, (int)f->height - tb_h());
    }
    g_gui_active = 0;
    accent_set(g_accent_idx);

    /* Real timing: calibrate the TSC, read the monitor's refresh rate from
     * EDID and make it the default frame limit. */
    tsc_init();
    g_panel_hz = display_panel_refresh_hz();
    if (g_panel_hz) {
        int found = -1;
        for (int i = 0; i < g_hz_count; ++i) if (g_hz_opts[i] == g_panel_hz) found = i;
        if (found < 0 && g_hz_count < HZ_MAX && g_panel_hz <= 1000) {
            int at = g_hz_count;
            while (at > 0 && g_hz_opts[at - 1] > g_panel_hz) { g_hz_opts[at] = g_hz_opts[at - 1]; --at; }
            g_hz_opts[at] = (uint16_t)g_panel_hz;
            ++g_hz_count;
            found = at;
        }
        if (found >= 0) g_hz_idx = found;
    }
    hz_set(g_hz_idx);
    term_reset();
}

void gui_start(void) {
    if (!fb) return;
    g_gui_active = 1;
    g_menu_open = 0;
    g_drag_id = 0;
    g_icon_sel = 0;
    g_hover = 0;
    g_last_clock_second = (uint64_t)-1;
    g_next_frame_clk = 0;
    g_locked = 1;
    g_unlock_anim_active = 0;
    g_unlock_anim = 0;
    g_prev_buttons = mouse_get_buttons();
    clock_refresh();
    gui_draw_desktop();   /* first frame immediately, then paced */
}

void gui_exit(void) { g_gui_active = 0; g_menu_open = 0; }
void gui_lock(void) {
    if (g_gui_active) {
        g_unlock_anim_active = 0;
        g_unlock_anim = 0;
        g_locked = 1;
        g_menu_open = 0;
        g_lock_input_len = 0;
        g_lock_input[0] = 0;
        request_redraw();
    }
}

/* 1 when a frame is due within ~2 ms: the main loop then polls instead of
 * halting, so a 144/240 Hz limit is not rounded to the 1 kHz timer tick. */
int gui_wants_spin(void) {
    if (!g_gui_active || !g_scene_dirty) return 0;
    uint64_t now = pace_now();
    if (now >= g_next_frame_clk) return 1;
    return (g_next_frame_clk - now) <= (2u * pace_hz() / 1000u);
}
int gui_is_active(void) { return g_gui_active; }

void gui_status(const char *text) {
    g_gui_message = text ? text : "";
    g_msg_until_ms = pit_get_uptime_ms() + 4000;
    if (g_gui_active) request_redraw();
}

int gui_handle_key(int key) {
    if (!g_gui_active) return 0;
    request_redraw();

    if (g_locked) {
        if (!g_lock_passwd_enabled) {
            gui_begin_unlock();
            return 1;
        }
        /* Password mode: handle PIN input */
        if (key == '\b') {
            if (g_lock_input_len > 0) { g_lock_input[--g_lock_input_len] = 0; g_lock_error = ""; }
        } else if (key == '\n' || key == '\r' || key == 13) {
            g_lock_input[g_lock_input_len] = 0;
            /* Compare input with stored password */
            int match = 1;
            if (g_lock_input_len != g_lock_password_len) match = 0;
            else { for (int i = 0; i < g_lock_input_len; i++) if (g_lock_input[i] != g_lock_password[i]) { match = 0; break; } }
            if (match) {
                g_lock_error = "";
                g_lock_input_len = 0; g_lock_input[0] = 0;
                gui_begin_unlock();
            } else {
                g_lock_error = "Wrong PIN";
                g_lock_input_len = 0; g_lock_input[0] = 0;
            }
        } else if (key >= 32 && key < 127 && g_lock_input_len < 31) {
            g_lock_input[g_lock_input_len++] = (char)key;
            g_lock_input[g_lock_input_len] = 0;
            g_lock_error = "";
        }
        request_redraw();
        return 1;
    }

    if (key == '\t') { if (g_menu_open) g_menu_open = 0; else menu_open(); return 1; }

    if (g_menu_open) {
        int list[NAPPS]; int n = menu_list(list);
        if (key == GUI_KEY_ESCAPE) { g_menu_open = 0; return 1; }
        if (key == GUI_KEY_UP) { if (n) g_menu_sel = (g_menu_sel + n - 1) % n; return 1; }
        if (key == GUI_KEY_DOWN) { if (n) g_menu_sel = (g_menu_sel + 1) % n; return 1; }
        if (key == GUI_KEY_ENTER || key == '\n') { if (n) { launch_app(list[g_menu_sel < n ? g_menu_sel : 0]); g_menu_open = 0; } return 1; }
        if (key == '\b') { if (g_search_len) g_search_query[--g_search_len] = 0; g_menu_sel = 0; return 1; }
        if (key >= 32 && key < 127 && g_search_len < (int)sizeof(g_search_query) - 1) {
            g_search_query[g_search_len++] = (char)key; g_search_query[g_search_len] = 0; g_menu_sel = 0;
        }
        return 1;
    }

    int aid = gui_window_active_id();
    if (aid) {
        if (key == GUI_KEY_ESCAPE) { gui_window_close((gui_window_id_t)aid); return 1; }
        if (aid == GUI_WINDOW_TERMINAL) {
            if (key == GUI_KEY_ENTER || key == '\n') { term_exec(); return 1; }
            if (key == '\b') { if (g_term_len) g_term_line[--g_term_len] = 0; return 1; }
            if (key >= 32 && key < 127 && g_term_len < (int)sizeof(g_term_line) - 1) { g_term_line[g_term_len++] = (char)key; g_term_line[g_term_len] = 0; }
            return 1;
        }
        if (aid == GUI_WINDOW_FILES) {
            char names[24][VFS_NAME_LEN]; unsigned char dirs[24];
            int total = vfs_gui_list(names, dirs, 24) + 1;
            if (key == GUI_KEY_UP || key == 'w' || key == 'W') g_files_item = (g_files_item + total - 1) % total;
            else if (key == GUI_KEY_DOWN || key == 's' || key == 'S') g_files_item = (g_files_item + 1) % total;
            else if (key == GUI_KEY_ENTER || key == '\n') files_open_selected();
            else if (key == '\b') { vfs_cd(".."); g_files_item = 0; g_files_top = 0; }
            return 1;
        }
        if (aid == GUI_WINDOW_SETTINGS) {
            if (key == GUI_KEY_UP || key == 'w' || key == 'W') g_settings_item = (g_settings_item + SET_CATS - 1) % SET_CATS;
            else if (key == GUI_KEY_DOWN || key == 's' || key == 'S') g_settings_item = (g_settings_item + 1) % SET_CATS;
            else if (g_settings_item == 1 && (key == GUI_KEY_LEFT || key == GUI_KEY_RIGHT))
                hz_set(key == GUI_KEY_LEFT ? (g_hz_idx + HZ_COUNT - 1) % HZ_COUNT : (g_hz_idx + 1) % HZ_COUNT);
            else if (g_settings_item == 2 && (key == GUI_KEY_LEFT || key == GUI_KEY_RIGHT))
                wall_select(key == GUI_KEY_LEFT ? (g_wall_idx + WALL_COUNT - 1) % WALL_COUNT : (g_wall_idx + 1) % WALL_COUNT);
            return 1;
        }
        if (aid == GUI_WINDOW_CALC) {
            if (key == GUI_KEY_ENTER || key == '\n' || key == '=') calc_press('=');
            else if (key == '\b') calc_press('B');
            else if (key == 'c' || key == 'C') calc_press('C');
            else if (key == 'x' || key == 'X') calc_press('*');
            else if ((key >= '0' && key <= '9') || key == '.' || key == '+' || key == '-' || key == '*' || key == '/' || key == '%')
                calc_press(key);
            return 1;
        }
        if (aid == GUI_WINDOW_NOTES) {
            if (key == GUI_KEY_ENTER || key == '\n') { if (g_note_len < NOTE_CAP) { g_note[g_note_len++] = '\n'; g_note[g_note_len] = 0; } }
            else if (key == '\b') { if (g_note_len) g_note[--g_note_len] = 0; }
            else if (key >= 32 && key < 127 && g_note_len < NOTE_CAP) { g_note[g_note_len++] = (char)key; g_note[g_note_len] = 0; }
            return 1;
        }
        return 1;
    }

    /* No active window: keyboard control of the desktop icons. */
    if (key == GUI_KEY_ESCAPE) { gui_exit(); return 1; }
    if (key == '/' || key == 'q' || key == 'Q') { menu_open(); return 1; }
    if (key == GUI_KEY_UP || key == 'w' || key == 'W' || key == GUI_KEY_LEFT || key == 'a' || key == 'A')
        { g_icon_sel = g_icon_sel <= 0 ? NAPPS - 1 : g_icon_sel - 1; return 1; }
    if (key == GUI_KEY_DOWN || key == 's' || key == 'S' || key == GUI_KEY_RIGHT || key == 'd' || key == 'D')
        { g_icon_sel = (g_icon_sel + 1) % NAPPS; return 1; }
    if (key == GUI_KEY_ENTER || key == '\n') { launch_app(g_icon_sel < 0 ? 0 : g_icon_sel); return 1; }
    return 1;
}

/* Draws the whole scene into the back buffer, copies it to the screen and
 * puts the pointer on top. */
void gui_draw_desktop(void) {
    if (!fb) return;
    g_scene_dirty = 0;
    g_cursor_visible = 0;   /* the new frame overwrites whatever pointer was on screen */
    draw_scene();
    gui_present();
    draw_cursor();
    g_frame_count++;
    g_fps_frames++;
}

void gui_update(void) {
    if (!g_gui_active || !fb) return;

    int redraw = 0;
    int moved = mouse_has_moved();
    if (moved) mouse_clear_moved();
    int wheel = mouse_get_wheel();
    if (wheel) mouse_clear_wheel();

    int mx = mouse_get_x(), my = mouse_get_y();
    uint8_t buttons = mouse_get_buttons();
    uint8_t pressed = buttons & (uint8_t)~g_prev_buttons;
    g_prev_buttons = buttons;

    tb_sync();
    {   /* Click unlock is available only when no PIN is configured. */
        int was_locked = g_locked;
        if (g_locked && pressed && !g_lock_passwd_enabled) {
            gui_begin_unlock();
            redraw = 1;
        }
        if (was_locked) { moved = 0; wheel = 0; pressed = 0; }
    }
    if (!(buttons & 1)) g_drag_id = 0;
    if (moved) {
        int hv = compute_hover(mx, my);
        if (hv != g_hover) { g_hover = hv; redraw = 1; }
        if (g_drag_id && (buttons & 1)) { gui_window_move((gui_window_id_t)g_drag_id, mx - g_drag_dx, my - g_drag_dy); redraw = 1; }
    }
    if (wheel && ui_wheel(wheel)) redraw = 1;
    if (pressed & 1) {
        if (ui_click(mx, my)) redraw = 1;
        int hv = compute_hover(mx, my);
        if (hv != g_hover) g_hover = hv;
    }

    uint64_t now = pit_get_uptime_seconds();
    if (now != g_last_clock_second) {
        g_last_clock_second = now;
        g_fps = g_fps_frames; g_fps_frames = 0;
        int om = g_clock_m;
        clock_refresh();
        if (om != g_clock_m) redraw = 1;
        gui_window_t *sw = gui_window_get(GUI_WINDOW_SETTINGS);
        if (sw && !sw->minimized && g_settings_item == 0) redraw = 1;
    }
    if (g_unlock_anim_active) {
        uint64_t now_anim = pace_now();
        uint64_t hz = pace_hz();
        uint64_t duration = hz / 3u; /* about 333 ms */
        if (duration == 0) duration = 1;
        uint64_t elapsed = now_anim - g_unlock_anim_start;
        if (elapsed >= duration) {
            g_unlock_anim = 20;
            g_unlock_anim_active = 0;
        } else {
            /* Ease-out cubic: fast initial reveal, gentle finish. */
            uint64_t linear = (elapsed * 20u) / duration;
            uint64_t inv = 20u - linear;
            uint64_t eased = 20u - (inv * inv * inv) / 400u;
            g_unlock_anim = (int)(eased > 20u ? 20u : eased);
        }
        redraw = 1;
    }
    if (g_show_fps) redraw = 1;     /* a visible FPS counter needs live frames */
    if (g_gui_message && g_gui_message[0] && pit_get_uptime_ms() > g_msg_until_ms) { g_gui_message = ""; redraw = 1; }
    if (redraw) request_redraw();

    /* Frame pacing (rate chosen in Settings): render when something changed AND the frame slot
     * has come. Several changes inside one slot cost a single redraw. */
    uint64_t now_t = pace_now();
    if (g_scene_dirty && now_t >= g_next_frame_clk) {
        gui_draw_desktop();
        g_next_frame_clk += g_frame_clk;
        /* If we fell behind, do not burst to catch up - resync. */
        if (g_next_frame_clk + g_frame_clk < now_t) g_next_frame_clk = now_t + g_frame_clk;
    } else if (mouse_get_x() != g_cursor_x || mouse_get_y() != g_cursor_y || !g_cursor_visible) {
        /* Pointer-only change: touch just the pixels around it. Not
         * paced, so the cursor follows the mouse with minimal latency. */
        draw_cursor();
    }
}