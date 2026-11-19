#include "a64.h"
#include "gs101_disp.h"

/* Активный канал: окно DECON0 включено (CON_WIN бит0), его канал = CON_WIN[6:4],
 * и теневой адрес Y8 ненулевой. Выбирается первое подходящее окно. */
int disp_find_channel(struct disp_state *s) {
    s->ch = -1; s->win = -1;
    s->sysmmu_on = 0;
    for (int i = 0; i < 3; i++)
        if (rd32(SYSMMU_DPU0 + 0x10000ULL * i) & 1u) s->sysmmu_on |= 1u << i;

    for (int w = 0; w < 6; w++) {
        uint32_t cw = rd32(DECON0_WINCON + R_CON_WIN(w));
        if (!(cw & 1u)) continue;
        int ch = (int)((cw >> 4) & 7u);
        if (ch >= DPU_DMA_CH_MAX) continue;
        uint64_t a = rd32(dma_base(ch) + R_RDMA_BASEADDR_Y8 + DMA_SHD_OFFSET);
        if (!a) continue;
        uint32_t sz = rd32(dma_base(ch) + R_RDMA_SRC_SIZE + DMA_SHD_OFFSET);
        uint32_t ctl = rd32(dma_base(ch) + R_RDMA_IN_CTRL_0 + DMA_SHD_OFFSET);
        uint32_t st  = rd32(dma_base(ch) + R_RDMA_SRC_STRIDE_0 + DMA_SHD_OFFSET);
        s->ch = ch; s->win = w; s->fb_phys = a;
        s->width = sz & 0xFFFFu; s->height = sz >> 16;
        s->fmt = (ctl >> 8) & 0x3Fu;
        /* STRIDE_SEL (бит 20) включён -> шаг в байтах в [15:0], иначе плотная упаковка */
        s->pitch = (st & (1u << 20)) ? (st & 0xFFFFu) : s->width * 4u;
        return 0;
    }
    return -1;
}
