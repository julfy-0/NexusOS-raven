#include "a64.h"
#include "ramoops_state.h"

void ramoops_puts(const char *s) {
    while (*s && ramoops_pos < RAMOOPS_DATA_MAX) {
        ramoops_buf->data[ramoops_pos++] = (uint8_t)*s++;
    }
    ramoops_buf->size = ramoops_pos;
}
