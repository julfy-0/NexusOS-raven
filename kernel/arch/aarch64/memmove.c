#include "a64.h"
void *memmove(void *d, const void *s, size_t n) {
    uint8_t *dd = d; const uint8_t *ss = s;
    if (dd <= ss || dd >= ss + n) { while (n--) *dd++ = *ss++; }
    else { while (n--) dd[n] = ss[n]; }
    return d;
}
