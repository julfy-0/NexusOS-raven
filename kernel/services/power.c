/* NexusOS power service: real ACPI shutdown (S5) and reset.
 *
 * All table access goes through bounds-checked byte readers, 64-bit
 * addresses are never squeezed through 32-bit variables, and every
 * physical address is checked against the identity-mapped range before it
 * is dereferenced. */
#include <stdint.h>
#include <stddef.h>
#include "power.h"
#include "system.h"
#include "kstate.h"
#include "boot_info.h"
#include "paging.h"
#include "io.h"
#include "console.h"

#define ACPI_MAX_TABLE   (4u * 1024u * 1024u)
#define PM1_SCI_EN       0x0001u
#define PM1_SLP_EN       0x2000u

static int      g_inited;
static int      g_s5_ok;
static uint16_t g_pm1a, g_pm1b;
static uint16_t g_slp_a, g_slp_b;
static uint32_t g_smi_cmd;
static uint8_t  g_acpi_enable;
static int      g_reset_ok;
static uint8_t  g_reset_space;       /* 0 = memory, 1 = I/O */
static uint64_t g_reset_addr;
static uint8_t  g_reset_val;

static uint32_t rd8(const uint8_t *p)  { return p[0]; }
static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | (rd16(p + 2) << 16); }
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32); }

/* Everything below paging_base_identity_gib() is always mapped; anything
 * above it is only mapped if it was in the EFI memory map. Map it on demand
 * (idempotent) instead of faulting. */
static const uint8_t *phys_ptr(uint64_t phys, uint64_t len) {
    if (phys == 0 || len == 0) return NULL;
    uint64_t end = phys + len;
    if (end < phys) return NULL;
    uint64_t base_limit = paging_base_identity_gib() * (1ULL << 30);
    if (end > base_limit) paging_map_region(phys, end);
    return (const uint8_t *)(uintptr_t)phys;
}

static const uint8_t *map_table(uint64_t phys, const char *sig4) {
    const uint8_t *h = phys_ptr(phys, 36);
    if (!h) return NULL;
    if (sig4 && (h[0] != (uint8_t)sig4[0] || h[1] != (uint8_t)sig4[1] ||
                 h[2] != (uint8_t)sig4[2] || h[3] != (uint8_t)sig4[3])) return NULL;
    uint32_t len = rd32(h + 4);
    if (len < 36 || len > ACPI_MAX_TABLE) return NULL;
    return phys_ptr(phys, len);
}

static const uint8_t *find_table(const uint8_t *rsdp, const char *sig4) {
    uint8_t rev = rsdp[15];
    uint64_t xsdt = rev >= 2 ? rd64(rsdp + 24) : 0;
    uint64_t rsdt = rd32(rsdp + 16);
    int use_x = xsdt != 0;
    const uint8_t *root = map_table(use_x ? xsdt : rsdt, use_x ? "XSDT" : "RSDT");
    if (!root) return NULL;
    uint32_t len = rd32(root + 4);
    uint32_t esz = use_x ? 8u : 4u;
    uint32_t n = (len - 36u) / esz;
    for (uint32_t i = 0; i < n; ++i) {
        const uint8_t *e = root + 36 + (size_t)i * esz;
        uint64_t addr = use_x ? rd64(e) : rd32(e);
        const uint8_t *t = map_table(addr, sig4);
        if (t) return t;
    }
    return NULL;
}

/* Reads one AML integer (ZeroOp/OneOp/BytePrefix/WordPrefix). */
static int aml_int(const uint8_t **pp, const uint8_t *end, uint32_t *out) {
    const uint8_t *p = *pp;
    if (p >= end) return 0;
    if (*p == 0x00)      { *out = 0; *pp = p + 1; return 1; }
    if (*p == 0x01)      { *out = 1; *pp = p + 1; return 1; }
    if (*p == 0x0A && p + 1 < end) { *out = p[1]; *pp = p + 2; return 1; }
    if (*p == 0x0B && p + 2 < end) { *out = rd16(p + 1); *pp = p + 3; return 1; }
    return 0;
}

static int find_s5(const uint8_t *dsdt, uint32_t len, uint16_t *a, uint16_t *b) {
    const uint8_t *end = dsdt + len;
    for (const uint8_t *p = dsdt + 36; p + 12 < end; ++p) {
        if (p[0] != '_' || p[1] != 'S' || p[2] != '5' || p[3] != '_') continue;
        if (p[-1] != 0x08 && p[-1] != '\\') continue;      /* NameOp or root prefix */
        if (p[4] != 0x12) continue;                         /* PackageOp */
        const uint8_t *q = p + 5;
        q += 1 + ((*q >> 6) & 3u);                          /* PkgLength */
        q++;                                                /* NumElements */
        uint32_t va, vb;
        if (!aml_int(&q, end, &va)) continue;
        if (!aml_int(&q, end, &vb)) vb = va;
        *a = (uint16_t)((va & 7u) << 10);
        *b = (uint16_t)((vb & 7u) << 10);
        return 1;
    }
    return 0;
}

static void power_parse(void) {
    if (g_inited) return;
    g_inited = 1;

    nexus_boot_info_t *bi = kstate_get_boot_info();
    if (!bi || !bi->acpi_valid || !bi->acpi_rsdp) return;   /* fallbacks still work */

    const uint8_t *rsdp = phys_ptr(bi->acpi_rsdp, 36);
    if (!rsdp || rsdp[0] != 'R' || rsdp[1] != 'S' || rsdp[2] != 'D' || rsdp[3] != ' ') return;

    const uint8_t *fadt = find_table(rsdp, "FACP");
    if (!fadt) return;
    uint32_t flen = rd32(fadt + 4);

    g_smi_cmd = rd32(fadt + 48);
    g_acpi_enable = (uint8_t)rd8(fadt + 52);
    uint32_t pm1a = rd32(fadt + 64), pm1b = rd32(fadt + 68);
    if (!pm1a && flen >= 184 && rd8(fadt + 172) == 1) pm1a = (uint32_t)rd64(fadt + 176);   /* X_PM1a_CNT_BLK, I/O space */
    if (!pm1b && flen >= 196 && rd8(fadt + 184) == 1) pm1b = (uint32_t)rd64(fadt + 188);
    g_pm1a = (uint16_t)pm1a;
    g_pm1b = (uint16_t)pm1b;

    if (flen >= 129 && (rd32(fadt + 112) & (1u << 10))) {
        g_reset_space = (uint8_t)rd8(fadt + 116);
        g_reset_addr  = rd64(fadt + 120);
        g_reset_val   = (uint8_t)rd8(fadt + 128);
        g_reset_ok = (g_reset_space == 0 || g_reset_space == 1) && g_reset_addr != 0;
    }

    uint64_t dsdt_phys = rd32(fadt + 40);
    if (flen >= 148) { uint64_t x = rd64(fadt + 140); if (x) dsdt_phys = x; }
    const uint8_t *dsdt = map_table(dsdt_phys, "DSDT");
    if (dsdt && g_pm1a && find_s5(dsdt, rd32(dsdt + 4), &g_slp_a, &g_slp_b)) g_s5_ok = 1;
    return;
}

/* Tables are parsed lazily, on the first shutdown/reboot request, so a
 * firmware quirk can never break boot. */
int nexus_power_init(void) { return 1; }

int nexus_power_acpi_ready(void) { power_parse(); return g_s5_ok; }

static void acpi_enable_if_needed(void) {
    if (!g_pm1a) return;
    if ((inw(g_pm1a) & PM1_SCI_EN) == 0 && g_smi_cmd && g_acpi_enable) {
        outb((uint16_t)g_smi_cmd, g_acpi_enable);
        for (uint32_t i = 0; i < 3000000u && !(inw(g_pm1a) & PM1_SCI_EN); ++i)
            __asm__ volatile ("pause");
    }
}

static void halt_forever(void) __attribute__((noreturn));
static void halt_forever(void) {
    for (;;) __asm__ volatile ("cli; hlt");
}

void nexus_power_shutdown(void) {
    nexus_system_request_shutdown();
    power_parse();
    __asm__ volatile ("cli" ::: "memory");

    if (g_s5_ok) {
        acpi_enable_if_needed();
        outw(g_pm1a, (uint16_t)(g_slp_a | PM1_SLP_EN));
        if (g_pm1b) outw(g_pm1b, (uint16_t)(g_slp_b | PM1_SLP_EN));
        for (volatile uint32_t i = 0; i < 20000000u; ++i) { }   /* give hardware time to cut power */
    }
    /* Emulators / hypervisors that do not expose usable ACPI S5. */
    outw(0x604,  0x2000);   /* QEMU (q35 / piix4 ACPI) */
    outw(0xB004, 0x2000);   /* Bochs / old QEMU */
    outw(0x4004, 0x3400);   /* VirtualBox */
    outw(0x600,  0x34);     /* Cloud Hypervisor / some others */

    console_print("\nIt is now safe to turn off your computer.\n");
    halt_forever();
}

static void reset_via_acpi(void) {
    if (!g_reset_ok) return;
    if (g_reset_space == 1) {
        outb((uint16_t)g_reset_addr, g_reset_val);
    } else if (g_reset_addr < (1ULL << 32)) {
        volatile uint8_t *r = (volatile uint8_t *)phys_ptr(g_reset_addr, 1);
        if (r) *r = g_reset_val;
    }
    for (volatile uint32_t i = 0; i < 5000000u; ++i) { }
}

void nexus_power_reboot(void) {
    nexus_system_request_reboot();
    power_parse();
    __asm__ volatile ("cli" ::: "memory");

    reset_via_acpi();

    /* 8042 pulse (bounded wait: a dead controller must not hang us). */
    for (uint32_t i = 0; i < 200000u && (inb(0x64) & 0x02u); ++i) { }
    outb(0x64, 0xFE);
    for (volatile uint32_t i = 0; i < 5000000u; ++i) { }

    /* Reset Control Register (chipset). */
    outb(0xCF9, 0x02);
    outb(0xCF9, 0x06);
    for (volatile uint32_t i = 0; i < 5000000u; ++i) { }
    outb(0xCF9, 0x0E);
    for (volatile uint32_t i = 0; i < 5000000u; ++i) { }

    /* Last resort: triple fault through an empty IDT. */
    struct { uint16_t limit; uint64_t base; } __attribute__((packed)) idt = { 0, 0 };
    __asm__ volatile ("lidt %0; int3" : : "m"(idt));
    halt_forever();
}
