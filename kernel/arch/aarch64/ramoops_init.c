#include "a64.h"
#include "ramoops_state.h"

struct ramoops_buf *ramoops_buf;
uint32_t ramoops_pos;

void ramoops_init(void) {
    ramoops_buf = (struct ramoops_buf *)(uintptr_t)RAMOOPS_BASE;
    ramoops_buf->sig = 0x43474244u;     /* "DBGC": PERSISTENT_RAM_SIG ^ 0 для console */
    ramoops_buf->start = 0;
    ramoops_buf->size = 0;
    ramoops_pos = 0;
}
