#include "display.h"
#include "edid.h"
#include "gpu.h"
#include "nvidia_rtx.h"

static char g_gpu_desc[96];

static void put(char **p, const char *s) { while (*s && *p < g_gpu_desc + sizeof(g_gpu_desc) - 1) *(*p)++ = *s++; }

void display_get_status(display_status_t *o) {
    o->active_hz = display_panel_refresh_hz();
    o->panel_max_hz = display_panel_max_refresh_hz();
    o->modeset_available = 0;
    o->below_panel_max = (o->active_hz && o->panel_max_hz && o->panel_max_hz > o->active_hz + 1);
    o->backend = "UEFI GOP (refresh chosen by firmware)";
    o->modeset_note = "no GPU mode-setting driver";
    o->gpu_desc = "";

    const nvidia_rtx_info_t *r = nvidia_rtx_info();
    if (r->claimed) {
        o->modeset_available = nvidia_rtx_modeset_available();
        o->modeset_note = nvidia_rtx_modeset_note();
        char *p = g_gpu_desc;
        put(&p, nvidia_gpu_device_name(r->device_id));
        put(&p, " (");
        put(&p, nvidia_arch_name(r->arch));
        put(&p, ")");
        *p = 0;
        o->gpu_desc = g_gpu_desc;
    }
}

int display_set_refresh_hz(uint32_t hz) {
    display_status_t s;
    display_get_status(&s);
    if (s.active_hz && hz == s.active_hz) return DISPLAY_OK;
    if (s.panel_max_hz && hz > s.panel_max_hz) return DISPLAY_ERR_RANGE;
    /* No backend implements an actual switch yet; even a driver that reports
     * modeset_available must add its call here. */
    return DISPLAY_ERR_UNSUPPORTED;
}
