/* NexusOS: захват дисплея gs101 (Pixel 6 Pro), режим "унаследовать от ABL".
 * Адреса: arch/arm64/boot/dts/google/gs101-drm-dpu.dtsi, gs101-sysmmu.dtsi.
 * Смещения регистров: google-modules/display samsung/cal_9845/regs-decon.h,
 * regs-dpp.h (ветка android-gs-raviole-5.10-android13, GPL-2.0, (c) Samsung/Google). */
#ifndef NEXUS_GS101_DISP_H
#define NEXUS_GS101_DISP_H
#include <stdint.h>
#include <stddef.h>
#include "nexus_version.h"
#define GS101_DISP_VERSION NEXUS_VERSION_STRING

#define DECON0_MAIN     0x1C300000ULL
#define DECON0_WINCON   0x1C330000ULL
#define DSIM0_BASE      0x1C2C0000ULL      /* MIPI DSI Controller 0 */
#define DPU_DMA_BASE    0x1C0B0000ULL      /* L0; канал n = +n*0x1000 */
#define DPP_L0_BASE     0x1C0D0000ULL      /* DPP Layer 0 */
#define DPP_L1_BASE     0x1C0D1000ULL      /* DPP Layer 1 */
#define DPU_DMA_CH_MAX  6
#define SYSMMU_DPU0     0x1C100000ULL      /* dpu1 +0x10000, dpu2 +0x20000 */

/* ── Параметры панели Pixel 6 Pro (gs101-raven-display.dtsi) ────────── */
#define PANEL_RAVEN_NAME    "samsung,s6e3hc3"
#define PANEL_RAVEN_WIDTH   1440
#define PANEL_RAVEN_HEIGHT  3120
#define PANEL_REFRESH_60HZ  60
#define PANEL_REFRESH_120HZ 120

/* DECON main */
#define R_GLOBAL_CON    0x0020
#define R_TRIG_CON      0x0030
#define R_SHD_REG_UP_REQ 0x0050
#define R_DATA_PATH_CON_0 0x0200
#define R_BLD_BG_IMG_SIZE_PRI 0x0220
#define R_OF_SIZE_0     0x0290
#define GLOBAL_CON_RUN_STATUS  (1u << 4)
#define GLOBAL_CON_CMD_MODE    (1u << 8)
#define SW_TRIG_EN      (1u << 8)
#define SW_TRIG_DET_EN  (1u << 1)
#define HW_TRIG_EN      (1u << 0)
#define HW_TRIG_MASK_DECON (1u << 4)
#define SHD_UP_GLOBAL   (1u << 31)
#define SHD_UP_WIN(w)   (1u << (w))
/* WINCON: DECON_CON_WIN(n) = 0x1000*n, бит0 = enable, [6:4] = канал DMA */
#define R_CON_WIN(n)    (0x1000 * (n))

/* IDMA (DPU_DMA) */
#define R_RDMA_ENABLE   0x0000
#define R_RDMA_IRQ      0x0004
#define R_RDMA_IN_CTRL_0 0x0008
#define R_RDMA_SRC_SIZE 0x0010
#define R_RDMA_IMG_SIZE 0x0018
#define R_RDMA_BASEADDR_Y8 0x0040
#define R_RDMA_SRC_STRIDE_0 0x0050
#define DMA_SHD_OFFSET  0x0400             /* теневая копия = то, что сейчас сканируется */

struct disp_state {
    int      ch;         /* активный канал DPU_DMA, -1 если не найден */
    int      win;        /* окно DECON0, к которому привязан канал */
    uint64_t fb_phys;    /* адрес буфера, который сейчас сканируется */
    uint32_t width, height;
    uint32_t pitch;      /* байт на строку */
    uint32_t fmt;        /* код IDMA_IMG_FORMAT_* */
    uint32_t sysmmu_on;  /* маска включённых SysMMU dpu0..2 */
    volatile uint32_t *fb;
};

static inline uint32_t rd32(uint64_t a) { return *(volatile uint32_t *)(uintptr_t)a; }
static inline void wr32(uint64_t a, uint32_t v) { *(volatile uint32_t *)(uintptr_t)a = v; }
static inline uint64_t dma_base(int ch) { return DPU_DMA_BASE + 0x1000ULL * (uint64_t)ch; }

void disp_log_reg(const char *name, uint64_t addr);
void disp_probe(struct disp_state *s);
int  disp_find_channel(struct disp_state *s);
int  disp_map_fb(struct disp_state *s);
uint32_t disp_pack(uint32_t fmt, uint8_t r, uint8_t g, uint8_t b);
uint32_t disp_pack_rgb(uint32_t fmt, uint32_t rgb);
void disp_unpack(uint32_t fmt, uint32_t pixel, uint8_t *r, uint8_t *g, uint8_t *b);
void disp_present(struct disp_state *s);
#endif
