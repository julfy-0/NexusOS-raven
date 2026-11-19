#ifndef NEXUS_GS101_DISP_UI_H
#define NEXUS_GS101_DISP_UI_H

#include "gs101_disp.h"

struct nexus_sysinfo {
    uint64_t current_el;
    uint64_t load_base;
    uint64_t dtb;
    uint64_t timer_freq;
    uint64_t dram_base;
    uint64_t dram_size;
};

void disp_ui_draw(struct disp_state *s, const struct nexus_sysinfo *info);
void disp_ui_update_heartbeat(struct disp_state *s, uint32_t seconds);
int  disp_ui_handle_touch(struct disp_state *s, int tx, int ty, int pressed);

#endif
