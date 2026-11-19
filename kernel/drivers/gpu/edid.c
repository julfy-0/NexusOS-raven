/* NexusOS: bounded EDID parser for base + CTA-861 extension detailed timings. */
#include "edid.h"
#include "kstate.h"
#include "boot_info.h"

static int block_checksum_ok(const uint8_t *e) {
    uint8_t sum = 0;
    for (int i = 0; i < 128; ++i) sum = (uint8_t)(sum + e[i]);
    return sum == 0;
}

static int edid_valid(const uint8_t *e, uint32_t size) {
    static const uint8_t magic[8] = {0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00};
    if (!e || size < 128 || size > NEXUS_EDID_MAX_SIZE || (size % 128)) return 0;
    for (int i = 0; i < 8; ++i) if (e[i] != magic[i]) return 0;
    if (!block_checksum_ok(e)) return 0;
    uint32_t blocks = size / 128;
    uint32_t advertised = (uint32_t)e[126] + 1;
    if (blocks > advertised) blocks = advertised;
    for (uint32_t b = 1; b < blocks; ++b) {
        const uint8_t *x = e + b * 128;
        if (!block_checksum_ok(x)) continue; /* ignore a damaged extension */
    }
    return 1;
}

static uint32_t dtd_hz(const uint8_t *d, uint32_t *ha_out, uint32_t *va_out) {
    uint32_t pclk10k = (uint32_t)d[0] | ((uint32_t)d[1] << 8);
    if (!pclk10k) return 0;
    uint32_t ha = (uint32_t)d[2] | (((uint32_t)d[4] & 0xF0u) << 4);
    uint32_t hb = (uint32_t)d[3] | (((uint32_t)d[4] & 0x0Fu) << 8);
    uint32_t va = (uint32_t)d[5] | (((uint32_t)d[7] & 0xF0u) << 4);
    uint32_t vb = (uint32_t)d[6] | (((uint32_t)d[7] & 0x0Fu) << 8);
    uint64_t total = (uint64_t)(ha + hb) * (va + vb);
    if (!total) return 0;
    uint64_t clock = (uint64_t)pclk10k * 10000ULL;
    uint32_t hz = (uint32_t)((clock + total / 2) / total);
    if (hz < 20 || hz > 1000) return 0;
    if (ha_out) *ha_out = ha;
    if (va_out) *va_out = va;
    return hz;
}

/* Visit detailed timings in the base block and CTA-861 extension blocks. */
static void scan_timings(const uint8_t *e, uint32_t size, uint32_t w, uint32_t h,
                         uint32_t *first, uint32_t *match, uint32_t *maximum) {
    uint32_t blocks = size / 128;
    uint32_t advertised = (uint32_t)e[126] + 1;
    if (blocks > advertised) blocks = advertised;
    for (uint32_t b = 0; b < blocks; ++b) {
        const uint8_t *blk = e + b * 128;
        if (b && !block_checksum_ok(blk)) continue;
        uint32_t start = 54, end = 126;
        if (b && blk[0] == 0x02) { /* CTA-861 */
            uint32_t dtd_offset = blk[2];
            if (dtd_offset < 4 || dtd_offset > 127) continue;
            start = dtd_offset;
            end = 127;
        } else if (b) {
            continue; /* unsupported extension type; do not misparse it as DTD */
        }
        for (uint32_t off = start; off + 18 <= end; off += 18) {
            uint32_t tw = 0, th = 0;
            uint32_t hz = dtd_hz(blk + off, &tw, &th);
            if (!hz) continue;
            if (!*first) *first = hz;
            if (!*match && tw == w && th == h) *match = hz;
            if (hz > *maximum) *maximum = hz;
        }
    }
}

uint32_t edid_refresh_hz(const uint8_t *e, uint32_t size, uint32_t w, uint32_t h) {
    if (!edid_valid(e, size)) return 0;
    uint32_t first = 0, match = 0, maximum = 0;
    scan_timings(e, size, w, h, &first, &match, &maximum);
    return match ? match : first;
}

uint32_t edid_max_refresh_hz(const uint8_t *e, uint32_t size) {
    if (!edid_valid(e, size)) return 0;
    uint32_t first = 0, match = 0, maximum = 0;
    scan_timings(e, size, 0, 0, &first, &match, &maximum);

    /* EDID base-block Display Range Limits descriptor (0xFD). This is a
     * capability range, not proof that every resolution supports that rate. */
    int rev14 = e[18] == 1 && e[19] >= 4;
    for (uint32_t off = 54; off + 18 <= 126; off += 18) {
        const uint8_t *d = e + off;
        if (d[0] == 0 && d[1] == 0 && d[3] == 0xFD) {
            uint32_t mx = d[6];
            if (rev14 && (d[4] & 0x02u)) mx += 255;
            if (mx >= 20 && mx <= 1000 && mx > maximum) maximum = mx;
        }
    }
    return maximum;
}

static uint32_t g_panel_hz, g_panel_max_hz;
static int g_panel_hz_done, g_panel_max_hz_done;

uint32_t display_panel_refresh_hz(void) {
    if (g_panel_hz_done) return g_panel_hz;
    g_panel_hz_done = 1;
    nexus_boot_info_t *bi = kstate_get_boot_info();
    if (bi && bi->edid_valid)
        g_panel_hz = edid_refresh_hz(bi->edid, bi->edid_size, bi->fb.width, bi->fb.height);
    return g_panel_hz;
}

uint32_t display_panel_max_refresh_hz(void) {
    if (g_panel_max_hz_done) return g_panel_max_hz;
    g_panel_max_hz_done = 1;
    nexus_boot_info_t *bi = kstate_get_boot_info();
    if (bi && bi->edid_valid)
        g_panel_max_hz = edid_max_refresh_hz(bi->edid, bi->edid_size);
    return g_panel_max_hz;
}
