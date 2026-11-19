#include "a64.h"
#include "gs101_disp.h"

/* Печатает "name@addr=val"; строка пишется ДО чтения — при зависании шины
 * в ramoops останется имя регистра, на котором всё встало. */
void disp_log_reg(const char *name, uint64_t addr) {
    ramoops_puts(name); ramoops_puts("@"); ramoops_put_hex(addr); ramoops_puts("=");
    ramoops_flush();
    uint32_t v = rd32(addr);
    ramoops_put_hex(v); ramoops_puts("\n");
    ramoops_flush();
}
