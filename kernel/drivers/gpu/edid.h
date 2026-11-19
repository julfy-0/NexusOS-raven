#ifndef NEXUSOS_EDID_H
#define NEXUSOS_EDID_H

#include <stdint.h>

/* EDID 1.4 base block plus up to three validated extension blocks. */
#define NEXUS_EDID_MAX_SIZE 512u

uint32_t edid_refresh_hz(const uint8_t *edid, uint32_t edid_size,
                         uint32_t width, uint32_t height);
uint32_t edid_max_refresh_hz(const uint8_t *edid, uint32_t edid_size);
uint32_t display_panel_max_refresh_hz(void);
uint32_t display_panel_refresh_hz(void);

#endif
