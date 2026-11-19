#include "a64.h"

void ramoops_put_hex(uint64_t v) {
    char b[19]; b[0] = '0'; b[1] = 'x';
    for (int i = 0; i < 16; i++) b[2 + i] = "0123456789abcdef"[(v >> ((15 - i) * 4)) & 15];
    b[18] = 0;
    ramoops_puts(b);
}
