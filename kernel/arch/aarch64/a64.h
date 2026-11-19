#ifndef NEXUS_A64_H
#define NEXUS_A64_H
#include <stdint.h>
#include <stddef.h>

/* Версия служб ядра на экране загрузки (конвенция проекта: службы 1.0, драйверы = версия NexusOS). */
#define NEXUS_SERVICE_VERSION "1.0"

#ifndef RAMOOPS_BASE
#define RAMOOPS_BASE   0xFD3FF000ULL   /* gs101-rmem.dtsi: ramoops_mem@fd3ff000 */
#endif
#define RAMOOPS_CONSOLE_SIZE 0x200000ULL

void  mmu_init(uint64_t load_base);
void  mmu_map_2m(uint64_t phys, int attr);
void  ramoops_init(void);
void  ramoops_puts(const char *s);
void  ramoops_put_hex(uint64_t v);
void  ramoops_flush(void);
void  exc_report(uint64_t type, uint64_t esr, uint64_t elr, uint64_t far);
void  psci_system_reset(void);
void  delay_sec(uint32_t s);
void  delay_ms(uint32_t ms);
uint64_t timer_freq(void);
int   fdt_first_memory(const void *fdt, uint64_t *base, uint64_t *size);

#define MMU_ATTR_DEVICE 0
#define MMU_ATTR_NC     1
#define MMU_ATTR_WB     2

void *memcpy(void *d, const void *s, size_t n);
void *memset(void *d, int v, size_t n);
void *memmove(void *d, const void *s, size_t n);
int   memcmp(const void *a, const void *b, size_t n);
struct disp_state;
void exc_set_display(struct disp_state *s);
#endif
