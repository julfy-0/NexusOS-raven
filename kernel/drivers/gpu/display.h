#ifndef NEXUSOS_DISPLAY_H
#define NEXUSOS_DISPLAY_H
#include <stdint.h>

/* Display refresh status.
 *
 * "Hz" and "FPS" are different things here:
 *   - refresh (Hz) is how often the GPU scans the framebuffer out to the
 *     monitor. It is programmed by the GPU mode-setting driver, or - today -
 *     chosen by the UEFI firmware from the monitor's EDID.
 *   - the GUI frame limit (FPS cap) only decides how often NexusOS renders a
 *     new frame. It cannot make a monitor refresh faster.
 */
typedef struct {
    uint32_t active_hz;        /* refresh the firmware is driving (EDID timing, 0 = unknown) */
    uint32_t panel_max_hz;     /* highest refresh the panel advertises (0 = unknown)         */
    int      modeset_available;/* 1 if NexusOS can change the refresh rate itself            */
    int      below_panel_max;  /* panel supports more Hz than are active                     */
    const char *backend;       /* who owns scan-out                                           */
    const char *modeset_note;  /* why switching is (un)available                              */
    const char *gpu_desc;      /* e.g. "GeForce RTX 4060 (Ada Lovelace)" or "" if no RTX     */
} display_status_t;

#define DISPLAY_OK            0
#define DISPLAY_ERR_UNSUPPORTED (-1)
#define DISPLAY_ERR_RANGE       (-2)

void display_get_status(display_status_t *out);

/* Requests a real change of the monitor refresh rate. EDID detection now
 * includes CTA extension timings, but actual switching still requires a GPU
 * mode-setting backend. Returns DISPLAY_ERR_UNSUPPORTED until one is present. */
int display_set_refresh_hz(uint32_t hz);

#endif
