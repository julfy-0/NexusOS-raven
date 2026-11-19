#include "a64.h"

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static uint64_t be64(const uint8_t *p) { return ((uint64_t)be32(p) << 32) | be32(p + 4); }

static int streq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* Читает n ячеек (1 или 2) big-endian. */
static uint64_t cells(const uint8_t *p, uint32_t n) { return n >= 2 ? be64(p) : be32(p); }

/*
 * Ищет первый узел memory[@...] в корне и читает reg с учётом #address-cells / #size-cells
 * корня (по умолчанию 2/1; у gs101 именно 2/1, то есть запись reg = 12 байт, а не 16).
 */
int fdt_first_memory(const void *fdt, uint64_t *base, uint64_t *size) {
    const uint8_t *h = fdt;
    if (be32(h) != 0xd00dfeedu) return -1;
    uint32_t off_struct = be32(h + 8), off_str = be32(h + 12);
    const uint8_t *p = h + off_struct;
    const char *strs = (const char *)(h + off_str);
    uint32_t ac = 2, sc = 1;
    int in_mem = 0, depth = 0;
    for (;;) {
        uint32_t tag = be32(p); p += 4;
        if (tag == 1) {
            const char *name = (const char *)p;
            uint32_t l = 0; while (name[l]) l++;
            in_mem = (depth == 1 && name[0]=='m' && name[1]=='e' && name[2]=='m' &&
                      name[3]=='o' && name[4]=='r' && name[5]=='y' &&
                      (name[6] == 0 || name[6] == '@'));
            depth++;
            p += (l + 1 + 3) & ~3u;
        } else if (tag == 2) {
            depth--; in_mem = 0;
            if (depth < 0) break;
        } else if (tag == 3) {
            uint32_t len = be32(p), nameoff = be32(p + 4); p += 8;
            const char *pn = strs + nameoff;
            if (depth == 1 && len == 4) {
                if (streq(pn, "#address-cells")) ac = be32(p);
                else if (streq(pn, "#size-cells")) sc = be32(p);
            }
            if (in_mem && streq(pn, "reg") && ac >= 1 && ac <= 2 && sc >= 1 && sc <= 2 &&
                len >= (ac + sc) * 4) {
                *base = cells(p, ac);
                *size = cells(p + ac * 4, sc);
                return 0;
            }
            p += (len + 3) & ~3u;
        } else if (tag == 4) {
        } else break;
    }
    return -1;
}