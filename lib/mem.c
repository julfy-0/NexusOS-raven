/* NexusOS: минимальная замена <string.h>. Freestanding — libc нет. */
#include <stddef.h>
#include <stdint.h>

/* Bulk copy/fill use string instructions (8 bytes per step, ERMS/fast-string
 * microcode on modern CPUs). The old byte loops made a full-screen present
 * (8 MiB at 1920x1080) take tens of milliseconds. The direction flag is
 * clear on entry per the SysV/UEFI ABIs. */
void *memcpy(void *dest, const void *src, size_t n) {
    void *ret = dest;
    size_t q = n >> 3, r = n & 7u;
    __asm__ volatile ("rep movsq" : "+D"(dest), "+S"(src), "+c"(q) : : "memory");
    __asm__ volatile ("rep movsb" : "+D"(dest), "+S"(src), "+c"(r) : : "memory");
    return ret;
}

void *memset(void *dest, int value, size_t n) {
    void *ret = dest;
    uint64_t v = (uint8_t)value;
    v |= v << 8; v |= v << 16; v |= v << 32;
    size_t q = n >> 3, r = n & 7u;
    __asm__ volatile ("rep stosq" : "+D"(dest), "+c"(q) : "a"(v) : "memory");
    __asm__ volatile ("rep stosb" : "+D"(dest), "+c"(r) : "a"(v) : "memory");
    return ret;
}

void *memmove(void *dest, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    if (d <= s || d >= s + n) return memcpy(dest, src, n);
    /* Overlapping with dest above src: copy backwards. */
    for (size_t i = n; i > 0; i--) d[i - 1] = s[i - 1];
    return dest;
}

int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    for (size_t i = 0; i < n; i++) {
        if (pa[i] != pb[i]) return (int)pa[i] - (int)pb[i];
    }
    return 0;
}

size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

int strcmp(const char *a, const char *b) {
    while (*a && (*a == *b)) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    while (n && *a && (*a == *b)) { a++; b++; n--; }
    if (n == 0) return 0;
    return (unsigned char)*a - (unsigned char)*b;
}
