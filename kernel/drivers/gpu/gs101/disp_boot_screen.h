#ifndef NEXUS_DISP_BOOT_SCREEN_H
#define NEXUS_DISP_BOOT_SCREEN_H

#include <stdint.h>
#include "gs101_disp.h"

void boot_svc(const char *name, const char *ver, int ok);
void boot_note(const char *s);
void boot_note_cat(const char *a, const char *b);
void boot_note_hex(const char *a, uint64_t v);
void boot_note_dec2(const char *a, uint32_t x, const char *mid, uint32_t y);
void boot_console_start(struct disp_state *ds);
void boot_console_finish(void);

#endif