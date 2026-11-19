/*
 * ufs_gs101.c — UFS HCI (Exynos/gs101) @ 0x14700000, polling, slot 0.
 * Работает поверх контроллера и линка, поднятых ABL. UCD/UTRL/буфер — в образе ядра (identity map).
 */
#include "a64.h"
#include "gs101_disp.h"     /* rd32 / wr32 */
#include "ufs_gs101.h"

#define UFS_BASE   0x14700000ULL
#define UFS_VS     0x14701100ULL      /* vs_hci */
#define VS_UTRL_NEXUS_TYPE 0x40       /* бит слота: 1 = SCSI-команда, 0 = device management (NOP/query) */

#define R_CAP      0x00
#define R_VER      0x08
#define R_IS       0x20
#define R_HCS      0x30
#define R_HCE      0x34
#define R_AHIT     0x18
#define R_UTRLBA   0x50
#define R_UTRLBAU  0x54
#define R_UTRLDBR  0x58
#define R_UTRLRSR  0x60

#define HCS_DP     (1U << 0)
#define HCS_UTRLRDY (1U << 1)
#define HCS_UCRDY  (1U << 3)
#define IS_FATAL   ((1U << 16) | (1U << 17) | (1U << 18))   /* HCFES | SBFES | CEFES */

#define BOUNCE_SZ  0x12000U            /* 72 КиБ: 17 блоков по 4 КиБ или 128 по 512 Б */

struct utrd { uint32_t d0, d1, d2, d3, ucdba, ucdbau, resp, prdt; };

static struct utrd g_utrl[32]            __attribute__((aligned(1024)));
static uint8_t     g_ucd[4096]           __attribute__((aligned(4096)));   /* 0: CMD UPIU, 512: RESP, 1024: PRDT */
static uint8_t     g_bounce[BOUNCE_SZ]   __attribute__((aligned(4096)));

static struct ufs_gs101_status g_st;
static uint32_t g_blksz[8];
static uint64_t g_blocks[8];
static uint8_t  g_lun;
static int      g_quiet;

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
static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
static uint64_t be64(const uint8_t *p) { return ((uint64_t)be32(p) << 32) | be32(p + 4); }
static void put_be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

static void err(const char *s) {
    g_st.last_error = s;
    if (g_quiet) return;
    ramoops_puts("[ufs] ERR "); ramoops_puts(s); ramoops_puts("\n");
}

static void list_restart(void) {
    wr32(UFS_BASE + R_UTRLRSR, 0);
    delay_ms(1);
    wr32(UFS_BASE + R_UTRLRSR, 1);
}

static void dbg_fail(const char *why) {
    if (g_quiet) return;
    ramoops_puts("[ufs] cmd fail: "); ramoops_puts(why);
    ramoops_puts(" IS="); ramoops_put_hex(rd32(UFS_BASE + R_IS));
    ramoops_puts(" HCS="); ramoops_put_hex(rd32(UFS_BASE + R_HCS));
    ramoops_puts(" DBR="); ramoops_put_hex(rd32(UFS_BASE + R_UTRLDBR));
    ramoops_puts(" NEXUS="); ramoops_put_hex(rd32(UFS_VS + VS_UTRL_NEXUS_TYPE));
    ramoops_puts(" UTRLBA="); ramoops_put_hex(rd32(UFS_BASE + R_UTRLBA));
    ramoops_puts(" OCS="); ramoops_put_hex(g_utrl[0].d2 & 0xFF);
    ramoops_puts("\n");
}

/* ── UPIU-команда в слоте 0 ──────────────────────────────────────────── */
/* cmdtype: 1 = SCSI/storage, 2 = device management (NOP).  dir: 0 нет, 1 читаем, 2 пишем.
 * Данные всегда через g_bounce.  Возврат: 0 ок, -1 таймаут/фатал, -2 OCS, -3 CHECK CONDITION, -4 UPIU-ответ. */
static int ufs_cmd(uint8_t cmdtype, uint8_t trans, uint8_t flags, uint8_t lun,
                   const uint8_t *cdb, uint32_t edtl, uint32_t dlen, int dir) {
    struct utrd *d = &g_utrl[0];
    uint64_t u = (uint64_t)(uintptr_t)g_ucd;

    memset(g_ucd, 0, 2048);
    g_ucd[0] = trans; g_ucd[1] = flags; g_ucd[2] = lun; g_ucd[3] = 0;
    put_be32(g_ucd + 12, edtl);
    if (cdb) memcpy(g_ucd + 16, cdb, 16);

    d->d0 = ((uint32_t)cmdtype << 28) | (dir == 1 ? 0x04000000U : (dir == 2 ? 0x02000000U : 0U)) | (1U << 24);
    d->d1 = 0; d->d2 = 0xF; d->d3 = 0;
    d->ucdba = lo32(u); d->ucdbau = hi32(u);
    d->resp = 128U | (128U << 16);                       /* длина | смещение, в dword */

    uint32_t nprd = 0;
    if (dlen) {
        uint64_t a = (uint64_t)(uintptr_t)g_bounce;
        uint32_t *p = (uint32_t *)(void *)(g_ucd + 1024);
        p[0] = lo32(a); p[1] = hi32(a); p[2] = 0; p[3] = dlen - 1;
        nprd = 1;
        if (dir == 2) dc_clean(g_bounce, dlen); else dc_inval(g_bounce, dlen);
    }
    d->prdt = nprd | (256U << 16);                       /* число PRD | смещение в dword */
    dc_clean(d, sizeof *d);
    dc_clean(g_ucd, 2048);

    /* как exynos_ufs_set_nexus_t_xfer_req(): для не-SCSI (NOP OUT) бит слота сбрасывается */
    {
        uint32_t nt = rd32(UFS_VS + VS_UTRL_NEXUS_TYPE);
        if (cmdtype == 1) nt |= 1U; else nt &= ~1U;
        wr32(UFS_VS + VS_UTRL_NEXUS_TYPE, nt);
    }

    wr32(UFS_BASE + R_IS, rd32(UFS_BASE + R_IS));
    dsb_sy();
    wr32(UFS_BASE + R_UTRLDBR, 1);

    uint64_t t0 = ticks(), lim = ms2t(3000);
    while (rd32(UFS_BASE + R_UTRLDBR) & 1) {
        if (rd32(UFS_BASE + R_IS) & IS_FATAL) { dbg_fail("fatal"); err("HCI fatal error"); list_restart(); return -1; }
        if (ticks() - t0 > lim) { dbg_fail("timeout"); err("command timeout"); list_restart(); return -1; }
    }
    wr32(UFS_BASE + R_IS, rd32(UFS_BASE + R_IS));

    dc_inval(d, 32);
    dc_inval(g_ucd + 512, 512);
    uint32_t ocs = d->d2 & 0xFF;
    if (ocs) {
        if (!g_quiet) { ramoops_puts("[ufs] OCS="); ramoops_put_hex(ocs); ramoops_puts("\n"); }
        err("transfer request error (OCS)");
        return -2;
    }
    if (dlen && dir == 1) dc_inval(g_bounce, dlen);

    const uint8_t *r = g_ucd + 512;
    if (r[6] != 0) { err("UPIU response failure"); return -4; }
    if (trans == 1 && r[7] != 0) {
        uint32_t sl = ((uint32_t)r[32] << 8) | r[33];
        g_st.sense_key = g_st.asc = g_st.ascq = 0;
        if (sl >= 14) { g_st.sense_key = r[34 + 2] & 0xF; g_st.asc = r[34 + 12]; g_st.ascq = r[34 + 13]; }
        err("SCSI CHECK CONDITION");
        return -3;
    }
    return 0;
}

static int scsi(uint8_t lun, const uint8_t *cdb, uint32_t len, int dir) {
    int rc = -1;
    for (int t = 0; t < 3; t++) {
        rc = ufs_cmd(1, 1, dir == 1 ? 0x40 : (dir == 2 ? 0x20 : 0x00), lun, cdb, len, len, dir);
        if (rc != -3 || g_st.sense_key != 6) break;       /* UNIT ATTENTION — повторяем */
    }
    return rc;
}

/* ── ёмкость и блочный обмен ─────────────────────────────────────────── */
static int lun_capacity(uint8_t lun, uint64_t *blocks, uint32_t *bs) {
    uint8_t cdb[16] = { 0x25 };
    int rc = scsi(lun, cdb, 8, 1);
    if (rc) return rc;
    uint32_t last = be32(g_bounce);
    *bs = be32(g_bounce + 4);
    *blocks = (uint64_t)last + 1;
    if (last == 0xFFFFFFFFU) {                              /* READ CAPACITY(16) */
        memset(cdb, 0, sizeof cdb);
        cdb[0] = 0x9E; cdb[1] = 0x10; put_be32(cdb + 10, 32);
        rc = scsi(lun, cdb, 32, 1);
        if (rc) return rc;
        *blocks = be64(g_bounce) + 1;
        *bs = be32(g_bounce + 8);
    }
    return (*bs && *blocks) ? 0 : -5;
}

static int blk_io(uint8_t lun, uint64_t lba, uint32_t nb, int write) {
    uint32_t bs = g_blksz[lun];
    if (!bs || !nb || nb > 0xFFFF || (uint64_t)nb * bs > sizeof g_bounce) return -9;
    if (lba + nb > g_blocks[lun] || lba > 0xFFFFFFFFULL) return -8;
    uint8_t cdb[16] = { write ? 0x2A : 0x28 };
    put_be32(cdb + 2, (uint32_t)lba);
    cdb[7] = (uint8_t)(nb >> 8); cdb[8] = (uint8_t)nb;
    return scsi(lun, cdb, nb * bs, write ? 2 : 1);
}

int ufs_gs101_read(uint8_t lun, uint64_t lba, uint32_t n, void *buf) {
    if (!g_st.ready || lun > 7 || !g_blksz[lun]) return -9;
    uint32_t bs = g_blksz[lun], maxb = sizeof g_bounce / bs;
    uint8_t *o = (uint8_t *)buf;
    while (n) {
        uint32_t c = n > maxb ? maxb : n;
        int rc = blk_io(lun, lba, c, 0);
        if (rc) return rc;
        memcpy(o, g_bounce, (size_t)c * bs);
        o += (size_t)c * bs; lba += c; n -= c;
    }
    return 0;
}

int ufs_gs101_write(uint8_t lun, uint64_t lba, uint32_t n, const void *buf) {
    if (!g_st.ready || lun > 7 || !g_blksz[lun]) return -9;
    uint32_t bs = g_blksz[lun], maxb = sizeof g_bounce / bs;
    const uint8_t *i = (const uint8_t *)buf;
    while (n) {
        uint32_t c = n > maxb ? maxb : n;
        memcpy(g_bounce, i, (size_t)c * bs);
        int rc = blk_io(lun, lba, c, 1);
        if (rc) return rc;
        i += (size_t)c * bs; lba += c; n -= c;
    }
    return 0;
}

int ufs_gs101_flush(uint8_t lun) {
    uint8_t cdb[16] = { 0x35 };
    return g_st.ready ? scsi(lun, cdb, 0, 0) : -9;
}

/* ── инициализация ───────────────────────────────────────────────────── */
int ufs_gs101_init(void) {
    ramoops_puts("[ufs] init HCI @ 0x14700000\n");
    g_st.last_error = "none";

    uint32_t cap = rd32(UFS_BASE + R_CAP), ver = rd32(UFS_BASE + R_VER);
    uint32_t hce = rd32(UFS_BASE + R_HCE), hcs = rd32(UFS_BASE + R_HCS);
    g_st.cap = cap; g_st.hci_ver = ver;
    ramoops_puts("[ufs] CAP="); ramoops_put_hex(cap);
    ramoops_puts(" VER="); ramoops_put_hex(ver);
    ramoops_puts(" HCE="); ramoops_put_hex(hce);
    ramoops_puts(" HCS="); ramoops_put_hex(hcs);
    ramoops_puts(" AHIT="); ramoops_put_hex(rd32(UFS_BASE + R_AHIT)); ramoops_puts("\n");

    if (cap == 0 || cap == 0xFFFFFFFFU) { err("HCI not readable (clock off?)"); return -1; }
    if (!(hce & 1)) { err("HCE=0: controller not enabled by bootloader"); return -2; }
    if (!(hcs & HCS_UCRDY) || !(hcs & HCS_DP)) { err("UIC not ready / no device present"); return -3; }

    for (int i = 0; i < 100 && rd32(UFS_BASE + R_UTRLDBR); i++) delay_ms(1);

    uint32_t nx = rd32(UFS_VS + VS_UTRL_NEXUS_TYPE);
    ramoops_puts("[ufs] UTRL_NEXUS_TYPE="); ramoops_put_hex(nx); ramoops_puts("\n");
    ramoops_puts("[ufs] vs: TXPRDT="); ramoops_put_hex(rd32(UFS_VS + 0x00));
    ramoops_puts(" RXPRDT="); ramoops_put_hex(rd32(UFS_VS + 0x04));
    ramoops_puts(" REORDER="); ramoops_put_hex(rd32(UFS_VS + 0x60));
    ramoops_puts(" AXIBURST="); ramoops_put_hex(rd32(UFS_VS + 0x6C));
    ramoops_puts(" HSI2_IOCC="); ramoops_put_hex(rd32(0x14420000ULL + 0x710)); ramoops_puts("\n");

    wr32(UFS_BASE + R_UTRLRSR, 0);
    delay_ms(1);
    memset(g_utrl, 0, sizeof g_utrl);
    dc_clean(g_utrl, sizeof g_utrl);
    uint64_t lb = (uint64_t)(uintptr_t)g_utrl;
    wr32(UFS_BASE + R_UTRLBA, lo32(lb));
    wr32(UFS_BASE + R_UTRLBAU, hi32(lb));
    wr32(UFS_BASE + R_IS, rd32(UFS_BASE + R_IS));
    wr32(UFS_BASE + R_UTRLRSR, 1);
    delay_ms(1);
    if (!(rd32(UFS_BASE + R_HCS) & HCS_UTRLRDY)) { err("UTRL not ready"); return -4; }

    if (ufs_cmd(2, 0x00, 0, 0, 0, 0, 0, 0)) { err("NOP OUT failed (link/UniPro?)"); return -5; }
    ramoops_puts("[ufs] NOP OUT ok\n");

    g_quiet = 1;
    int first = -1;
    for (uint8_t l = 0; l < 8; l++) {
        uint64_t bl = 0; uint32_t bs = 0;
        uint8_t cdb[16] = { 0x00 };
        scsi(l, cdb, 0, 0);                                  /* TEST UNIT READY, сбрасывает UA */
        if (lun_capacity(l, &bl, &bs) == 0) {
            g_blocks[l] = bl; g_blksz[l] = bs; g_st.lun_present[l] = 1;
            if (first < 0) first = l;
            g_quiet = 0;
            ramoops_puts("[ufs] LUN "); ramoops_put_hex(l);
            ramoops_puts(" blocks="); ramoops_put_hex(bl);
            ramoops_puts(" blksz="); ramoops_put_hex(bs); ramoops_puts("\n");
            g_quiet = 1;
        }
    }
    g_quiet = 0;
    if (first < 0) { err("no LUN responded"); return -6; }

    g_lun = (uint8_t)first;
    g_st.lun = g_lun; g_st.blksz = g_blksz[g_lun]; g_st.blocks = g_blocks[g_lun];
    g_st.ready = 1;
    ramoops_puts("[ufs] ready\n");
    ramoops_flush();
    return 0;
}

void ufs_gs101_get_status(struct ufs_gs101_status *o) { if (o) *o = g_st; }

/* ── адаптер usb_msc: секторы по 512 Б ───────────────────────────────── */
static uint32_t ratio(void) {
    uint32_t bs = g_blksz[g_lun];
    return (bs >= 512 && (bs % 512) == 0) ? bs / 512 : 0;
}

static uint32_t be_count(void) {
    uint32_t r = ratio();
    if (!g_st.ready || !r) return 0;
    uint64_t s = g_blocks[g_lun] * r;
    return s > 0xFFFFFFFFULL ? 0xFFFFFFFFU : (uint32_t)s;
}

static int be_read(uint32_t lba, uint32_t n, void *buf) {
    uint32_t r = ratio();
    if (!r || !n || n > 128) return -1;
    uint64_t first = lba / r, last = ((uint64_t)lba + n - 1) / r;
    uint32_t nb = (uint32_t)(last - first + 1);
    if (blk_io(g_lun, first, nb, 0)) return -1;
    memcpy(buf, g_bounce + (size_t)(lba % r) * 512, (size_t)n * 512);
    return 0;
}

static int be_write(uint32_t lba, uint32_t n, const void *buf) {
    uint32_t r = ratio();
    if (!r || !n || n > 128) return -1;
    uint64_t first = lba / r, last = ((uint64_t)lba + n - 1) / r;
    uint32_t nb = (uint32_t)(last - first + 1);
    int partial = (lba % r) || (((uint64_t)lba + n) % r);
    if (partial && blk_io(g_lun, first, nb, 0)) return -1;   /* read-modify-write */
    memcpy(g_bounce + (size_t)(lba % r) * 512, buf, (size_t)n * 512);
    return blk_io(g_lun, first, nb, 1) ? -1 : 0;
}

static int be_flush(void) { return ufs_gs101_flush(g_lun); }

static struct usb_msc_backend g_be = { be_count, be_read, be_write, be_flush, 0 };

const struct usb_msc_backend *ufs_gs101_msc_backend(uint8_t lun, int writable) {
    if (lun > 7 || !g_st.lun_present[lun]) {
        for (lun = 0; lun < 8 && !g_st.lun_present[lun]; lun++) ;
        if (lun > 7) lun = 0;
    }
    g_lun = lun;
    g_st.lun = lun; g_st.blksz = g_blksz[lun]; g_st.blocks = g_blocks[lun];
    g_be.writable = writable ? 1 : 0;
    return &g_be;
}