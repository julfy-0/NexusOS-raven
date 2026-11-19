#include "a64.h"
#include "gs101_disp.h"

void disp_probe(struct disp_state *s) {
    (void)s;
    ramoops_puts("[disp] --- DECON0 main ---\n");
    disp_log_reg("GLOBAL_CON", DECON0_MAIN + R_GLOBAL_CON);
    disp_log_reg("TRIG_CON", DECON0_MAIN + R_TRIG_CON);
    disp_log_reg("SHD_REG_UP_REQ", DECON0_MAIN + R_SHD_REG_UP_REQ);
    disp_log_reg("DATA_PATH_CON_0", DECON0_MAIN + R_DATA_PATH_CON_0);
    disp_log_reg("BLD_BG_IMG_SIZE", DECON0_MAIN + R_BLD_BG_IMG_SIZE_PRI);
    disp_log_reg("OF_SIZE_0", DECON0_MAIN + R_OF_SIZE_0);

    ramoops_puts("[disp] --- WINCON ---\n");
    for (int w = 0; w < 6; w++) disp_log_reg("CON_WIN", DECON0_WINCON + R_CON_WIN(w));

    ramoops_puts("[disp] --- SysMMU dpu0..2 CTRL ---\n");
    for (int i = 0; i < 3; i++) disp_log_reg("MMU_CTRL", SYSMMU_DPU0 + 0x10000ULL * i);

    ramoops_puts("[disp] --- IDMA channels ---\n");
    for (int c = 0; c < DPU_DMA_CH_MAX; c++) {
        ramoops_puts("ch="); ramoops_put_hex((uint64_t)c); ramoops_puts("\n");
        disp_log_reg(" ENABLE", dma_base(c) + R_RDMA_ENABLE);
        disp_log_reg(" IN_CTRL_0", dma_base(c) + R_RDMA_IN_CTRL_0);
        disp_log_reg(" SRC_SIZE", dma_base(c) + R_RDMA_SRC_SIZE);
        disp_log_reg(" IMG_SIZE", dma_base(c) + R_RDMA_IMG_SIZE);
        disp_log_reg(" STRIDE_0", dma_base(c) + R_RDMA_SRC_STRIDE_0);
        disp_log_reg(" ADDR_Y8", dma_base(c) + R_RDMA_BASEADDR_Y8);
        disp_log_reg(" ADDR_Y8_SHD", dma_base(c) + R_RDMA_BASEADDR_Y8 + DMA_SHD_OFFSET);
    }
}
