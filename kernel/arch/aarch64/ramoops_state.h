#ifndef NEXUS_RAMOOPS_STATE_H
#define NEXUS_RAMOOPS_STATE_H
#include <stdint.h>
struct ramoops_buf { volatile uint32_t sig, start, size; volatile uint8_t data[]; };
extern struct ramoops_buf *ramoops_buf;
extern uint32_t ramoops_pos;
#define RAMOOPS_DATA_MAX (0x200000u - 12u)
#endif
