#include "a64.h"
void *memset(void *d, int v, size_t n) {
    uint8_t *dd = d;
    while (n--) *dd++ = (uint8_t)v;
    return d;
}
