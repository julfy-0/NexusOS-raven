#include "gpuinfo.h"
#include "console.h"
#include "gpu.h"
#include "display.h"
#include "nvidia_rtx.h"
#include "paging.h"

static void print_hex16(uint16_t v) {
    const char *d = "0123456789ABCDEF";
    char s[5] = {d[(v >> 12) & 15], d[(v >> 8) & 15], d[(v >> 4) & 15], d[v & 15], 0};
    console_print(s);
}

static void print_hz(uint32_t hz) {
    if (!hz) { console_print("unknown"); return; }
    console_print_dec(hz); console_print(" Hz");
}

static void print_display_section(void) {
    display_status_t d;
    display_get_status(&d);
    const nvidia_rtx_info_t *r = nvidia_rtx_info();

    console_print("\nNVIDIA RTX driver\n");
    if (r->claimed) {
        console_print("  GPU:          "); console_print(d.gpu_desc); console_print("\n");
        console_print("  PCI:          ");
        console_print_dec(r->bus); console_print(":"); console_print_dec(r->device);
        console_print("."); console_print_dec(r->function); console_print("\n");
        console_print("  Chip ID:      ");
        if (r->regs_ok) { console_print_hex(r->chipset); console_print(r->hw_verified ? "  (verified from GPU)\n" : "  (does not match)\n"); }
        else console_print("unreadable (BAR0 not decoded) - accepted by PCI ID only\n");
        console_print("  Status:       claimed, read-only; scan-out stays on UEFI GOP\n");
    } else {
        console_print("  No RTX GPU claimed");
        if (r->unclaimed_nvidia) console_print(" (NVIDIA non-RTX GPU left on UEFI GOP)");
        console_print("\n");
    }

    console_print("\nDisplay\n");
    console_print("  Scan-out:     "); console_print(d.backend); console_print("\n");
    console_print("  Refresh now:  "); print_hz(d.active_hz); console_print("\n");
    console_print("  EDID max:     "); print_hz(d.panel_max_hz); console_print("\n");
    console_print("  Hz switching: ");
    console_print(d.modeset_available ? "available" : "unavailable - ");
    if (!d.modeset_available) console_print(d.modeset_note);
    console_print("\n");
    if (d.below_panel_max) {
        console_print("  Note:         panel supports ");
        print_hz(d.panel_max_hz);
        console_print(" but firmware drives ");
        print_hz(d.active_hz);
        console_print(". EDID reports capability; actual 144 Hz requires GPU mode-setting support.\n");
    }
    console_print("  Framebuffer:  ");
    console_print(paging_fb_is_wc() ? "write-combining (fast path)\n" : "write-back + clflush (slow path)\n");
}

void gpuinfo_run(void) {
    const nexus_gpu_info_t *g = gpu_get_info();
    if (!g) {
        console_print("gpuinfo: no PCI display controller detected\n");
        print_display_section();
        return;
    }

    console_print("GPU information\n");
    console_print("  Vendor:       "); console_print(g->vendor_name); console_print("\n");
    console_print("  Device:       "); console_print(g->device_name); console_print("\n");
    console_print("  PCI:          ");
    console_print_dec(g->bus); console_print(":");
    console_print_dec(g->device); console_print(".");
    console_print_dec(g->function); console_print("\n");
    console_print("  Vendor ID:    0x"); print_hex16(g->vendor_id); console_print("\n");
    console_print("  Device ID:    0x"); print_hex16(g->device_id); console_print("\n");
    console_print("  Class:        0x"); print_hex16((uint16_t)((g->class_code << 8) | g->subclass));
    console_print(" / 0x"); print_hex16(g->prog_if); console_print("\n");
    console_print("  BAR0:         0x"); console_print_hex(g->bar0); console_print("\n");
    console_print("  Detection:    READY\n");
    print_display_section();
}
