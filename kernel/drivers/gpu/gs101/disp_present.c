#include "a64.h"
#include "gs101_disp.h"

/* Запрос на новый кадр: shadow update окна + глобальный, затем разрешение триггера
 * (как decon_reg_update_req_and_unmask в cal_9845). Режим триггера оставляем как у ABL. */
void disp_present(struct disp_state *s) {
    uint32_t trig = rd32(DECON0_MAIN + R_TRIG_CON);
    wr32(DECON0_MAIN + R_SHD_REG_UP_REQ, SHD_UP_GLOBAL | SHD_UP_WIN((uint32_t)s->win));
    if (rd32(DECON0_MAIN + R_GLOBAL_CON) & GLOBAL_CON_CMD_MODE) {
        if (trig & HW_TRIG_EN) trig &= ~HW_TRIG_MASK_DECON;       /* HW (TE) триггер: размаскировать */
        else                   trig |= SW_TRIG_EN | SW_TRIG_DET_EN; /* SW триггер */
        wr32(DECON0_MAIN + R_TRIG_CON, trig);
    }
    ramoops_flush();
}
