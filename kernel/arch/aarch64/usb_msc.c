/*
 * usb_msc.c — USB Mass Storage (BOT + SCSI) в device-режиме, DWC3 @ 0x11110000, Pixel 6 Pro.
 * Только HS, один LUN, polling. Эндпоинты: EP0 (phys 0/1), EP1 OUT (phys 2), EP1 IN (phys 3).
 */
#include "a64.h"
#include "gs101_disp.h"     /* rd32 / wr32 */
#include "usb_msc.h"

#define USB_BASE      0x11110000ULL
#define GCTL          0xC110
#define GSNPSID       0xC120
#define GUSB2PHYCFG   0xC200
#define GUSB3PIPECTL  0xC2C0
#define GTXFIFOSIZ(n) (0xC300 + 4 * (n))
#define GRXFIFOSIZ0   0xC380
#define GEVNTADRLO    0xC400
#define GEVNTADRHI    0xC404
#define GEVNTSIZ      0xC408
#define GEVNTCOUNT    0xC40C
#define DCFG          0xC700
#define DCTL          0xC704
#define DEVTEN        0xC708
#define DSTS          0xC70C
#define DALEPENA      0xC720

#define DCTL_RS       (1U << 31)
#define DCTL_CSFTRST  (1U << 30)
#define PHY_SOFTRST   (1U << 31)
#define U2_SUSPHY     (1U << 6)
#define U3_SUSPHY     (1U << 17)
#define DSTS_HALTED   (1U << 22)

#define DEPCFG        1
#define DEPXFERCFG    2
#define DEPSTALL      4
#define DEPCSTALL     5
#define DEPSTRTXFER   6
#define DEPENDXFER    8
#define DEPSTARTCFG   9
#define DEPCMD_ACT    (1U << 10)
#define DEPCMD_FORCE  (1U << 11)

#define TRB_HWO (1U << 0)
#define TRB_LST (1U << 1)
#define TRB_ISP (1U << 10)
#define TRB_IOC (1U << 11)

#define EVWORDS 256
#define XBUF_SZ 65536U
#define RD_SECTORS 8192U

enum { P_NONE, P_CTRL, P_CBW, P_DATA };
enum { EP0_SETUP, EP0_DATA, EP0_STATUS };

struct ptrb { uint32_t bpl, bph, size, ctrl; uint8_t pad[48]; } __attribute__((aligned(64)));

static uint32_t    g_evbuf[EVWORDS]        __attribute__((aligned(64)));
static struct ptrb g_trb[4];
static uint8_t     g_setup[64]             __attribute__((aligned(64)));
static uint8_t     g_ctlbuf[64]            __attribute__((aligned(64)));
static uint8_t     g_cbw_buf[512]          __attribute__((aligned(64)));
static uint8_t     g_csw_buf[64]           __attribute__((aligned(64)));
static uint8_t     g_xbuf[XBUF_SZ]         __attribute__((aligned(64)));
static uint8_t     g_ram[RD_SECTORS * 512U] __attribute__((aligned(4096)));

static uint32_t g_evpos;
static volatile uint8_t  g_busy[4], g_done[4], g_purpose[4], g_halt[4];
static volatile uint32_t g_remain[4];
static uint8_t  g_res[4];

static int g_ep0_state, g_data_started, g_status_nrdy, g_status_ep, g_three;
static uint32_t g_reply_len;
static volatile int g_abort, g_cbw_ready, g_need_reset;
static int g_cfg, g_startcfg2, g_run;
static uint8_t g_sk, g_asc, g_ascq;

static const struct usb_msc_backend *g_be;
static struct usb_msc_status g_stat;

/* ── утилиты ─────────────────────────────────────────────────────────── */
static inline uint32_t lo32(uint64_t v) { return (uint32_t)v; }
static inline uint32_t hi32(uint64_t v) { return (uint32_t)(v >> 32); }
static inline void dsb_sy(void) { __asm__ volatile("dsb sy" ::: "memory"); }
static inline uint64_t ticks(void) { uint64_t t; __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(t)); return t; }
static uint64_t ms2t(uint32_t ms) { uint64_t f = timer_freq(); if (!f) f = 24576000ULL; return f * ms / 1000ULL; }

static void dc_clean(const void *p, uint32_t n) {
    uintptr_t a = (uintptr_t)p & ~63UL, e = (uintptr_t)p + n;
    for (; a < e; a += 64) __asm__ volatile("dc cvac, %0" :: "r"(a) : "memory");
    dsb_sy();
}
static void dc_inval(const void *p, uint32_t n) {
    uintptr_t a = (uintptr_t)p & ~63UL, e = (uintptr_t)p + n;
    for (; a < e; a += 64) __asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
    dsb_sy();
}
static uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
static void put_be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void put_le32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void put_le16(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; }
static void err(const char *s) { g_stat.last_error = s; ramoops_puts("[msc] ERR "); ramoops_puts(s); ramoops_puts("\n"); }

/* ── RAM-диск FAT16, 4 МиБ ───────────────────────────────────────────── */
static void rd_format(void) {
    static const char txt[] = "NexusOS: Pixel 6 Pro USB mass storage works.\r\n";
    uint8_t *b = g_ram;
    memset(g_ram, 0, 98 * 512);
    b[0] = 0xEB; b[1] = 0x3C; b[2] = 0x90; memcpy(b + 3, "NEXUSOS ", 8);
    put_le16(b + 11, 512); b[13] = 1; put_le16(b + 14, 1); b[16] = 2; put_le16(b + 17, 512);
    put_le16(b + 19, RD_SECTORS); b[21] = 0xF8; put_le16(b + 22, 32); put_le16(b + 24, 32); put_le16(b + 26, 64);
    b[36] = 0x80; b[38] = 0x29; put_le32(b + 39, 0x4E455853);
    memcpy(b + 43, "NEXUS DISK ", 11); memcpy(b + 54, "FAT16   ", 8);
    b[510] = 0x55; b[511] = 0xAA;
    for (int f = 0; f < 2; f++) {
        uint8_t *fat = g_ram + (uint32_t)(1 + f * 32) * 512;
        fat[0] = 0xF8; fat[1] = 0xFF; fat[2] = 0xFF; fat[3] = 0xFF; fat[4] = 0xFF; fat[5] = 0xFF;
    }
    uint8_t *root = g_ram + 65U * 512;
    memcpy(root, "NEXUS DISK ", 11); root[11] = 0x08;
    uint8_t *e = root + 32;
    memcpy(e, "README  TXT", 11); e[11] = 0x20;
    put_le16(e + 16, (46U << 9) | (11U << 5) | 19U);
    put_le16(e + 24, (46U << 9) | (11U << 5) | 19U);
    put_le16(e + 26, 2); put_le32(e + 28, sizeof txt - 1);
    memcpy(g_ram + 97U * 512, txt, sizeof txt - 1);
}
static uint32_t rd_count(void) { return RD_SECTORS; }
static int rd_read(uint32_t lba, uint32_t n, void *buf) {
    if ((uint64_t)lba + n > RD_SECTORS) return -1;
    memcpy(buf, g_ram + (uint64_t)lba * 512, (size_t)n * 512); return 0;
}
static int rd_write(uint32_t lba, uint32_t n, const void *buf) {
    if ((uint64_t)lba + n > RD_SECTORS) return -1;
    memcpy(g_ram + (uint64_t)lba * 512, buf, (size_t)n * 512); return 0;
}
static const struct usb_msc_backend g_ram_be = { rd_count, rd_read, rd_write, 0, 1 };

/* ── команды эндпоинтов ──────────────────────────────────────────────── */
static int dep_cmd(uint32_t phys, uint32_t cmd, uint32_t p0, uint32_t p1, uint32_t p2, uint32_t *res) {
    uint64_t r = USB_BASE + 0xC800 + 0x10ULL * phys;
    wr32(r + 8, p0); wr32(r + 4, p1); wr32(r + 0, p2);
    wr32(r + 0xC, cmd | DEPCMD_ACT);
    for (uint32_t i = 0; i < 2000000U; i++) {
        uint32_t v = rd32(r + 0xC);
        if (!(v & DEPCMD_ACT)) { if (res) *res = v; return ((v >> 12) & 0xF) ? -1 : 0; }
    }
    return -2;
}

static void ep_end(uint32_t phys) {
    if (!g_busy[phys]) return;
    dep_cmd(phys, DEPENDXFER | DEPCMD_FORCE | ((uint32_t)g_res[phys] << 16), 0, 0, 0, 0);
    g_busy[phys] = 0;
    delay_ms(1);
}

static int xfer_start(uint32_t phys, const void *buf, uint32_t len, uint32_t trbctl, uint32_t purpose) {
    struct ptrb *t = &g_trb[phys];
    uint64_t a = (uint64_t)(uintptr_t)buf, ta = (uint64_t)(uintptr_t)t;
    uint32_t res = 0;
    t->bpl = lo32(a); t->bph = hi32(a); t->size = len;
    t->ctrl = TRB_HWO | TRB_LST | TRB_IOC | TRB_ISP | (trbctl << 4);
    dc_clean(t, 16);
    g_done[phys] = 0; g_purpose[phys] = (uint8_t)purpose; g_busy[phys] = 1;
    if (dep_cmd(phys, DEPSTRTXFER, hi32(ta), lo32(ta), 0, &res)) { g_busy[phys] = 0; err("STRTXFER failed"); return -1; }
    g_res[phys] = (uint8_t)((res >> 16) & 0x7F);
    return 0;
}

/* ── EP0 ─────────────────────────────────────────────────────────────── */
static void ep0_arm_setup(void) {
    g_ep0_state = EP0_SETUP;
    dc_inval(g_setup, sizeof g_setup);
    xfer_start(0, g_setup, 8, 2, P_CTRL);
}
static void ep0_status(uint32_t ep) { xfer_start(ep, 0, 0, g_three ? 4 : 3, P_CTRL); }
static void ep0_stall(void) {
    ep_end(0); ep_end(1);
    dep_cmd(0, DEPSTALL, 0, 0, 0, 0);
    ep0_arm_setup();
}

static void arm_cbw(void) {
    if (!g_cfg || g_need_reset || g_busy[2] || g_halt[2]) return;
    dc_inval(g_cbw_buf, sizeof g_cbw_buf);
    xfer_start(2, g_cbw_buf, 512, 1, P_CBW);
}

static void bulk_config(void) {
    if (!g_startcfg2) { dep_cmd(2, DEPSTARTCFG | (2U << 16), 0, 0, 0, 0); g_startcfg2 = 1; }
    for (uint32_t phys = 2; phys <= 3; phys++) {
        uint32_t fifo = (phys & 1) ? (phys >> 1) : 0;
        dep_cmd(phys, DEPCFG, (2U << 1) | (512U << 3) | (fifo << 17),
                (1U << 8) | (1U << 10) | (phys << 25), 0, 0);
        dep_cmd(phys, DEPXFERCFG, 1, 0, 0, 0);
    }
    wr32(USB_BASE + DALEPENA, rd32(USB_BASE + DALEPENA) | 0xC);
}

static void set_config(int v) {
    ep_end(2); ep_end(3);
    g_halt[2] = g_halt[3] = 0;
    g_abort = 1; g_cbw_ready = 0; g_need_reset = 0;
    if (v) { bulk_config(); g_cfg = 1; g_stat.configured = 1; arm_cbw(); ramoops_puts("[msc] configured\n"); }
    else   { wr32(USB_BASE + DALEPENA, rd32(USB_BASE + DALEPENA) & ~0xCU); g_cfg = 0; g_stat.configured = 0; }
}

static void bot_reset(void) {
    ep_end(2); ep_end(3);
    dep_cmd(2, DEPCSTALL, 0, 0, 0, 0); dep_cmd(3, DEPCSTALL, 0, 0, 0, 0);
    g_halt[2] = g_halt[3] = 0; g_need_reset = 0; g_abort = 1; g_cbw_ready = 0;
    arm_cbw();
}

static int addr_to_phys(uint16_t a) {
    int p = (a & 0xF) * 2 + ((a & 0x80) ? 1 : 0);
    return (p == 2 || p == 3) ? p : -1;
}

static void reply(const void *p, uint32_t len, uint32_t wl) {
    if (len > wl) len = wl;
    if (len > sizeof g_ctlbuf) len = sizeof g_ctlbuf;
    memcpy(g_ctlbuf, p, len);
    dc_clean(g_ctlbuf, sizeof g_ctlbuf);
    g_reply_len = len;
}

static int get_descriptor(uint16_t wv, uint16_t wl) {
    static const uint8_t dev[18] = { 18, 1, 0x00, 0x02, 0, 0, 0, 64, 0x09, 0x12, 0x01, 0x00, 0x01, 0x00, 1, 2, 3, 1 };
    static const uint8_t qual[10] = { 10, 6, 0x00, 0x02, 0, 0, 0, 64, 1, 0 };
    static const uint8_t cfg[32] = {
        9, 2, 32, 0, 1, 1, 0, 0xC0, 50,
        9, 4, 0, 0, 2, 8, 6, 0x50, 0,
        7, 5, 0x81, 2, 0x00, 0x02, 0,
        7, 5, 0x01, 2, 0x00, 0x02, 0 };
    static const char *const str[4] = { 0, "NexusOS", "Pixel 6 Pro Storage", "NEXUS0001" };
    uint8_t t = wv >> 8, i = wv & 0xFF;
    if (t == 1) { reply(dev, sizeof dev, wl); return 1; }
    if (t == 2 && i == 0) { reply(cfg, sizeof cfg, wl); return 1; }
    if (t == 6) { reply(qual, sizeof qual, wl); return 1; }
    if (t == 3) {
        uint8_t s[64];
        if (i == 0) { s[0] = 4; s[1] = 3; s[2] = 0x09; s[3] = 0x04; reply(s, 4, wl); return 1; }
        if (i <= 3) {
            uint32_t n = 2; const char *p = str[i];
            while (*p && n < 62) { s[n++] = (uint8_t)*p++; s[n++] = 0; }
            s[0] = (uint8_t)n; s[1] = 3; reply(s, n, wl); return 1;
        }
    }
    return 0;
}

static int std_req(uint8_t bm, uint8_t rq, uint16_t wv, uint16_t wi, uint16_t wl) {
    uint8_t r = bm & 0x1F; uint8_t b[2] = { 0, 0 };
    switch (rq) {
    case 0:  if (r == 0) b[0] = 1;
             else if (r == 2) { int p = addr_to_phys(wi); if (p < 0) return 0; b[0] = g_halt[p]; }
             reply(b, 2, wl); return 1;
    case 1:  if (r == 2 && wv == 0) {
                 int p = addr_to_phys(wi); if (p < 0) return 0;
                 dep_cmd((uint32_t)p, DEPCSTALL, 0, 0, 0, 0); g_halt[p] = 0; arm_cbw();
             }
             return 1;
    case 3:  if (r == 2 && wv == 0) {
                 int p = addr_to_phys(wi); if (p < 0) return 0;
                 dep_cmd((uint32_t)p, DEPSTALL, 0, 0, 0, 0); g_halt[p] = 1;
             }
             return 1;
    case 5:  wr32(USB_BASE + DCFG, (rd32(USB_BASE + DCFG) & ~(0x7FU << 3)) | ((uint32_t)(wv & 0x7F) << 3)); return 1;
    case 6:  return get_descriptor(wv, wl);
    case 8:  b[0] = (uint8_t)g_cfg; reply(b, 1, wl); return 1;
    case 9:  if ((wv & 0xFF) > 1) return 0; set_config(wv & 0xFF); return 1;
    case 10: reply(b, 1, wl); return 1;
    case 11: return wv == 0;
    default: return 0;
    }
}

static int cls_req(uint8_t bm, uint8_t rq, uint16_t wv, uint16_t wi, uint16_t wl) {
    uint8_t z = 0;
    if (bm == 0xA1 && rq == 0xFE && wv == 0 && wi == 0 && wl == 1) { reply(&z, 1, wl); return 1; }
    if (bm == 0x21 && rq == 0xFF && wl == 0) { bot_reset(); return 1; }
    return 0;
}

static void ctrl_setup(void) {
    dc_inval(g_setup, sizeof g_setup);
    uint8_t bm = g_setup[0], rq = g_setup[1];
    uint16_t wv = g_setup[2] | (g_setup[3] << 8), wi = g_setup[4] | (g_setup[5] << 8), wl = g_setup[6] | (g_setup[7] << 8);
    int ok;
    g_reply_len = 0;
    if (wl && !(bm & 0x80)) ok = 0;
    else if ((bm & 0x60) == 0x00) ok = std_req(bm, rq, wv, wi, wl);
    else if ((bm & 0x60) == 0x20) ok = cls_req(bm, rq, wv, wi, wl);
    else ok = 0;
    if (!ok) { ep0_stall(); return; }
    g_status_nrdy = 0;
    if (wl) { g_ep0_state = EP0_DATA; g_data_started = 0; g_three = 1; }
    else    { g_ep0_state = EP0_STATUS; g_three = 0; }
}

static void ep0_event(uint32_t phys, uint32_t type, uint32_t status) {
    if (type == 1) {
        g_busy[phys] = 0;
        if (g_ep0_state == EP0_SETUP) { if (phys == 0) ctrl_setup(); }
        else if (g_ep0_state == EP0_DATA) {
            g_ep0_state = EP0_STATUS;
            if (g_status_nrdy) { g_status_nrdy = 0; ep0_status((uint32_t)g_status_ep); }
        } else ep0_arm_setup();
    } else if (type == 3) {
        uint32_t ph = status & 3;
        if (ph == 1 && g_ep0_state == EP0_DATA && !g_data_started) {
            g_data_started = 1;
            xfer_start(1, g_ctlbuf, g_reply_len, 5, P_CTRL);
        } else if (ph == 2) {
            if (g_ep0_state == EP0_STATUS) ep0_status(phys);
            else if (g_ep0_state == EP0_DATA) { g_status_nrdy = 1; g_status_ep = (int)phys; }
        }
    }
}

/* ── события ─────────────────────────────────────────────────────────── */
static void bad_cbw(void) {
    dep_cmd(2, DEPSTALL, 0, 0, 0, 0); dep_cmd(3, DEPSTALL, 0, 0, 0, 0);
    g_halt[2] = g_halt[3] = 1; g_need_reset = 1;
    err("invalid CBW -> stall, wait reset recovery");
}

static void bulk_event(uint32_t phys, uint32_t type) {
    if (type != 1) return;
    dc_inval(&g_trb[phys], 16);
    g_remain[phys] = g_trb[phys].size & 0xFFFFFFU;
    g_busy[phys] = 0;
    if (g_purpose[phys] == P_CBW) {
        uint32_t got = 512 - g_remain[phys];
        dc_inval(g_cbw_buf, sizeof g_cbw_buf);
        if (got == 31 && le32(g_cbw_buf) == 0x43425355U) g_cbw_ready = 1; else bad_cbw();
    } else g_done[phys] = 1;
}

static void dev_event(uint32_t code) {
    switch (code) {
    case 0:   /* DISCONNECT */
        ramoops_puts("[msc] disconnect\n");
        g_abort = 1; g_cbw_ready = 0; g_cfg = 0; g_stat.connected = 0; g_stat.configured = 0;
        for (uint32_t p = 0; p < 4; p++) ep_end(p);
        break;
    case 1:   /* USB RESET */
        ramoops_puts("[msc] usb reset\n");
        g_abort = 1; g_cbw_ready = 0; g_cfg = 0; g_startcfg2 = 0; g_need_reset = 0; g_stat.configured = 0;
        g_halt[2] = g_halt[3] = 0;
        wr32(USB_BASE + DCFG, rd32(USB_BASE + DCFG) & ~(0x7FU << 3));
        break;
    case 2: { /* CONNECT DONE */
        uint32_t sp = rd32(USB_BASE + DSTS) & 7;
        g_stat.speed = (uint8_t)sp; g_stat.connected = 1;
        ramoops_puts("[msc] connect done speed="); ramoops_put_hex(sp); ramoops_puts("\n");
        if (sp != 0) err("not high-speed (HS only)");
        for (uint32_t p = 0; p < 4; p++) ep_end(p);
        wr32(USB_BASE + DALEPENA, 0);
        dep_cmd(0, DEPSTARTCFG, 0, 0, 0, 0);
        dep_cmd(0, DEPCFG, (0U << 1) | (64U << 3), (1U << 8) | (1U << 10) | (0U << 25), 0, 0);
        dep_cmd(1, DEPCFG, (0U << 1) | (64U << 3), (1U << 8) | (1U << 10) | (1U << 25), 0, 0);
        dep_cmd(0, DEPXFERCFG, 1, 0, 0, 0); dep_cmd(1, DEPXFERCFG, 1, 0, 0, 0);
        wr32(USB_BASE + DALEPENA, 3);
        ep0_arm_setup();
        break; }
    default: break;
    }
}

static void handle_event(uint32_t e) {
    if (e & 1) { if (((e >> 1) & 0x7F) == 0) dev_event((e >> 8) & 0xF); return; }
    uint32_t phys = (e >> 1) & 0x1F, type = (e >> 6) & 0xF, status = (e >> 12) & 0xF;
    if (phys < 2) ep0_event(phys, type, status);
    else if (phys < 4) bulk_event(phys, type);
}

static void evt_pump(void) {
    uint32_t cnt = rd32(USB_BASE + GEVNTCOUNT) & 0xFFFF;
    if (!cnt) return;
    dc_inval(g_evbuf, sizeof g_evbuf);
    for (uint32_t i = 0; i < cnt / 4; i++) {
        uint32_t e = g_evbuf[g_evpos];
        g_evpos = (g_evpos + 1) % EVWORDS;
        handle_event(e);
    }
    wr32(USB_BASE + GEVNTCOUNT, cnt);
}

/* ── bulk-обмен (блокирующий, с выходом по reset/disconnect) ─────────── */
static int bulk_wait(uint32_t phys, uint32_t ms) {
    uint64_t t0 = ticks(), lim = ms2t(ms);
    while (!g_done[phys]) {
        evt_pump();
        if (g_abort) return -1;
        if (ticks() - t0 > lim) { ep_end(phys); err("bulk timeout"); return -2; }
    }
    g_done[phys] = 0;
    return 0;
}
static int send_in(const void *buf, uint32_t len) {
    if (len) dc_clean(buf, len);
    if (xfer_start(3, buf, len, 1, P_DATA)) return -1;
    return bulk_wait(3, 5000);
}
static int recv_out(void *buf, uint32_t want, uint32_t *got) {
    uint32_t sz = (want + 511U) & ~511U;
    dc_inval(buf, sz);
    if (xfer_start(2, buf, sz, 1, P_DATA)) return -1;
    int r = bulk_wait(2, 5000);
    if (r) return r;
    dc_inval(buf, sz);
    uint32_t g = sz - g_remain[2];
    *got = g > want ? want : g;
    return 0;
}
static void drain_out(uint32_t n) {
    while (n) {
        uint32_t c = n > XBUF_SZ ? XBUF_SZ : n, g = 0;
        if (recv_out(g_xbuf, c, &g) || g == 0) return;
        n -= g < n ? g : n;
    }
}

/* ── SCSI ────────────────────────────────────────────────────────────── */
static void set_sense(uint8_t k, uint8_t a, uint8_t q) { g_sk = k; g_asc = a; g_ascq = q; }

static int do_read(const uint8_t *cb, int din, uint32_t dlen, uint32_t *done) {
    uint32_t lba = be32(cb + 2), cnt = (cb[7] << 8) | cb[8];
    int st = 0;
    if (!din || dlen != cnt * 512U || (uint64_t)lba + cnt > g_be->block_count()) {
        set_sense(5, ((uint64_t)lba + cnt > g_be->block_count()) ? 0x21 : 0x24, 0);
        if (din && dlen) return send_in(g_xbuf, 0) ? -1 : 1;
        if (!din && dlen) drain_out(dlen);
        return 1;
    }
    while (cnt) {
        uint32_t c = cnt > 128 ? 128 : cnt;
        if (g_be->read(lba, c, g_xbuf)) { set_sense(3, 0x11, 0); st = 1; break; }
        if (send_in(g_xbuf, c * 512U)) return -1;
        *done += c * 512U; g_stat.read_blocks += c; lba += c; cnt -= c;
    }
    if (*done < dlen && (*done % 512U) == 0 && send_in(g_xbuf, 0)) return -1;
    return st;
}

static int do_write(const uint8_t *cb, int din, uint32_t dlen, uint32_t *done) {
    uint32_t lba = be32(cb + 2), cnt = (cb[7] << 8) | cb[8];
    int st = 0, drain = 0;
    if (din || dlen != cnt * 512U || (uint64_t)lba + cnt > g_be->block_count()) {
        set_sense(5, ((uint64_t)lba + cnt > g_be->block_count()) ? 0x21 : 0x24, 0);
        if (din && dlen) return send_in(g_xbuf, 0) ? -1 : 1;
        if (!din && dlen) drain_out(dlen);
        return 1;
    }
    if (!g_be->writable || !g_be->write) { set_sense(7, 0x27, 0); drain_out(dlen); return 1; }
    while (cnt) {
        uint32_t c = cnt > 128 ? 128 : cnt, got = 0;
        if (recv_out(g_xbuf, c * 512U, &got) || got != c * 512U) return -1;
        if (!drain && g_be->write(lba, c, g_xbuf)) { set_sense(3, 0x03, 0); st = 1; drain = 1; }
        if (!drain) { *done += c * 512U; g_stat.write_blocks += c; }
        lba += c; cnt -= c;
    }
    return st;
}

static void send_csw(uint32_t tag, uint32_t residue, uint32_t status) {
    put_le32(g_csw_buf, 0x53425355U); put_le32(g_csw_buf + 4, tag);
    put_le32(g_csw_buf + 8, residue); g_csw_buf[12] = (uint8_t)status;
    send_in(g_csw_buf, 13);
}

static void handle_cbw(void) {
    uint8_t c[31];
    for (int i = 0; i < 31; i++) c[i] = g_cbw_buf[i];
    g_cbw_ready = 0; g_abort = 0;
    uint32_t tag = le32(c + 4), dlen = le32(c + 8), n = 0, done = 0;
    int din = (c[12] & 0x80) != 0, st = 0;
    uint8_t cl = c[14] & 0x1F;
    const uint8_t *cb = c + 15;
    uint8_t op = cb[0];
    uint8_t *x = g_xbuf;

    if (c[13] != 0 || cl == 0 || cl > 16) { bad_cbw(); return; }
    g_stat.cmds++;

    switch (op) {
    case 0x00: case 0x1B: case 0x1E: case 0x2F: break;
    case 0x35: if (g_be->flush) g_be->flush(); break;
    case 0x03:
        memset(x, 0, 18); x[0] = 0x70; x[2] = g_sk; x[7] = 10; x[12] = g_asc; x[13] = g_ascq;
        set_sense(0, 0, 0); n = 18; break;
    case 0x12:
        if (cb[1] & 1) { set_sense(5, 0x24, 0); st = 1; break; }
        memset(x, 0, 36); x[0] = 0x00; x[1] = 0x80; x[2] = 0x05; x[3] = 0x02; x[4] = 31;
        memcpy(x + 8, "NexusOS ", 8); memcpy(x + 16, "Pixel 6 Pro Disk", 16); memcpy(x + 32, "0.1 ", 4);
        n = 36; break;
    case 0x1A: memset(x, 0, 4); x[0] = 3; x[2] = g_be->writable ? 0 : 0x80; n = 4; break;
    case 0x5A: memset(x, 0, 8); x[1] = 6; x[3] = g_be->writable ? 0 : 0x80; n = 8; break;
    case 0x25: put_be32(x, g_be->block_count() - 1); put_be32(x + 4, 512); n = 8; break;
    case 0x23:
        memset(x, 0, 12); x[3] = 8; put_be32(x + 4, g_be->block_count()); x[8] = 2; x[10] = 2; n = 12; break;
    case 0x28:
        st = do_read(cb, din, dlen, &done);
        if (st < 0) goto out;
        send_csw(tag, dlen - done, (uint32_t)st); goto out;
    case 0x2A:
        st = do_write(cb, din, dlen, &done);
        if (st < 0) goto out;
        send_csw(tag, dlen - done, (uint32_t)st); goto out;
    default: set_sense(5, 0x20, 0); st = 1; break;
    }

    if (din) {
        uint32_t ts = n < dlen ? n : dlen;
        if (ts && send_in(x, ts)) goto out;
        done = ts;
        if (done < dlen && (done % 512U) == 0 && send_in(x, 0)) goto out;
    } else if (dlen) drain_out(dlen);
    if (st == 0 && (op == 0x00)) set_sense(0, 0, 0);
    send_csw(tag, dlen - done, (uint32_t)st);
out:
    arm_cbw();
}

/* ── запуск DWC3 в device-режиме ─────────────────────────────────────── */
static int dwc3_dev_start(void) {
    uint32_t id = rd32(USB_BASE + GSNPSID);
    ramoops_puts("[msc] GSNPSID="); ramoops_put_hex(id); ramoops_puts("\n");
    if (id == 0 || id == 0xFFFFFFFFU || ((id >> 16) != 0x5533 && (id >> 16) != 0x3331)) {
        err("DWC3 not found (clock/PHY off?)"); return -1;
    }
    /* останавливаем то, что оставил бутлоадер (fastboot) */
    wr32(USB_BASE + DCTL, rd32(USB_BASE + DCTL) & ~DCTL_RS);
    for (int i = 0; i < 100 && !(rd32(USB_BASE + DSTS) & DSTS_HALTED); i++) delay_ms(1);

    uint32_t u2 = rd32(USB_BASE + GUSB2PHYCFG), u3 = rd32(USB_BASE + GUSB3PIPECTL);
    wr32(USB_BASE + GUSB3PIPECTL, u3 | PHY_SOFTRST);
    wr32(USB_BASE + GUSB2PHYCFG,  u2 | PHY_SOFTRST);
    wr32(USB_BASE + DCTL, DCTL_CSFTRST);
    for (int i = 0; i < 200 && (rd32(USB_BASE + DCTL) & DCTL_CSFTRST); i++) delay_ms(1);
    if (rd32(USB_BASE + DCTL) & DCTL_CSFTRST) { err("core soft reset timeout"); return -2; }
    delay_ms(50);
    wr32(USB_BASE + GUSB3PIPECTL, u3 & ~(PHY_SOFTRST | U3_SUSPHY));
    wr32(USB_BASE + GUSB2PHYCFG,  u2 & ~(PHY_SOFTRST | U2_SUSPHY));
    delay_ms(50);

    wr32(USB_BASE + GCTL, (rd32(USB_BASE + GCTL) & ~(3U << 12)) | (2U << 12));   /* device */
    delay_ms(20);

    ramoops_puts("[msc] FIFO tx0="); ramoops_put_hex(rd32(USB_BASE + GTXFIFOSIZ(0)));
    ramoops_puts(" tx1="); ramoops_put_hex(rd32(USB_BASE + GTXFIFOSIZ(1)));
    ramoops_puts(" rx0="); ramoops_put_hex(rd32(USB_BASE + GRXFIFOSIZ0)); ramoops_puts("\n");

    memset(g_evbuf, 0, sizeof g_evbuf);
    dc_clean(g_evbuf, sizeof g_evbuf);
    g_evpos = 0;
    wr32(USB_BASE + GEVNTADRLO, lo32((uint64_t)(uintptr_t)g_evbuf));
    wr32(USB_BASE + GEVNTADRHI, hi32((uint64_t)(uintptr_t)g_evbuf));
    wr32(USB_BASE + GEVNTSIZ, (sizeof g_evbuf) | (1U << 31));
    wr32(USB_BASE + GEVNTCOUNT, 0);

    wr32(USB_BASE + DCFG, rd32(USB_BASE + DCFG) & ~(0x7FU << 3) & ~7U);          /* HS, адрес 0 */
    wr32(USB_BASE + DEVTEN, (1U << 0) | (1U << 1) | (1U << 2));                  /* disc, reset, conndone */

    wr32(USB_BASE + DCTL, rd32(USB_BASE + DCTL) | DCTL_RS);
    for (int i = 0; i < 100 && (rd32(USB_BASE + DSTS) & DSTS_HALTED); i++) delay_ms(1);
    if (rd32(USB_BASE + DSTS) & DSTS_HALTED) { err("device run timeout"); return -3; }
    ramoops_puts("[msc] DWC3 device running, DSTS="); ramoops_put_hex(rd32(USB_BASE + DSTS)); ramoops_puts("\n");
    return 0;
}

/* ── API ─────────────────────────────────────────────────────────────── */
int usb_msc_init(const struct usb_msc_backend *b) {
    ramoops_puts("[msc] USB mass storage init\n");
    g_stat.last_error = "none";
    if (b) g_be = b; else { rd_format(); g_be = &g_ram_be; }
    if (dwc3_dev_start() < 0) return -1;
    g_stat.running = 1; g_run = 1;
    return 0;
}

void usb_msc_service(uint32_t ms) {
    if (!g_run) { delay_ms(ms); return; }
    uint64_t t0 = ticks(), lim = ms2t(ms);
    do {
        evt_pump();
        if (g_cbw_ready) handle_cbw();
    } while (ticks() - t0 < lim);
}

void usb_msc_get_status(struct usb_msc_status *o) { if (o) *o = g_stat; }