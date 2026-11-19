#include "inputinfo.h"
#include "console.h"
#include "keyboard.h"
#include "mouse.h"
#include "xhci.h"
#include "usb.h"
#include "drivers/input/input.h"

static void print_u64(uint64_t v) {
    if (v > 0xFFFFFFFFu) { console_print_dec((uint32_t)(v >> 32)); console_print(" (hi), "); }
    console_print_dec((uint32_t)v);
}

void inputinfo_run(void) {
    console_print("=== USB Host ===\n");
    console_print("  Host type:       "); console_print(usb_host_name());      console_print("\n");
    console_print("  PCI controllers: "); console_print_dec(usb_pci_controller_count()); console_print("\n");
    console_print("  xHCI count:      "); console_print_dec((uint32_t)usb_xhci_controller_count()); console_print("\n");
    console_print("  USB ports:       "); console_print_dec(usb_port_count());  console_print("\n");
    console_print("  Connected ports: "); console_print_dec(usb_connected_ports()); console_print("\n");
    console_print("  Last error:      "); console_print(usb_last_error());      console_print("\n");
    console_print("  xHCI error:      "); console_print(xhci_last_error());     console_print("\n");
    if (xhci_bar0()) {
        console_print("  xHCI BAR0:       0x"); console_print_hex(xhci_bar0()); console_print("\n");
        console_print("  xHCI PCI:        bus="); console_print_dec(xhci_pci_bus());
        console_print(" dev="); console_print_dec(xhci_pci_device());
        console_print(" fn="); console_print_dec(xhci_pci_function()); console_print("\n");
        console_print("  Vendor:Device:   "); console_print_hex(xhci_vendor_id());
        console_print(":"); console_print_hex(xhci_device_id()); console_print("\n");
    }

    console_print("\n=== Input Devices ===\n");
    console_print("  PS/2 keyboard:   "); console_print(input_source_present(NEXUS_INPUT_SOURCE_PS2, NEXUS_INPUT_DEVICE_KEYBOARD) ? "ready\n" : "not detected\n");
    console_print("  USB HID keyboard:"); console_print(xhci_keyboard_present() ? "ready\n" : "not detected\n");
    console_print("  PS/2 mouse:      "); console_print(input_source_present(NEXUS_INPUT_SOURCE_PS2, NEXUS_INPUT_DEVICE_MOUSE) ? "ready\n" : "not detected\n");
    console_print("  USB HID mouse:   "); console_print(xhci_mouse_present() ? "ready\n" : "not detected\n");

    console_print("\n=== Event Counters ===\n");
    console_print("  Kbd events PS/2: "); print_u64(input_keyboard_events(NEXUS_INPUT_SOURCE_PS2)); console_print("\n");
    console_print("  Kbd events USB:  "); print_u64(input_keyboard_events(NEXUS_INPUT_SOURCE_USB)); console_print("\n");
    console_print("  Mouse ev PS/2:   "); print_u64(input_mouse_events(NEXUS_INPUT_SOURCE_PS2));    console_print("\n");
    console_print("  Mouse ev USB:    "); print_u64(input_mouse_events(NEXUS_INPUT_SOURCE_USB));    console_print("\n");

    {
        uint32_t ok = 0, err = 0, cc = 0, dci = 0;
        xhci_debug_stats(&ok, &err, &cc, &dci);
        console_print("  xHCI xfer ok:    "); console_print_dec(ok);  console_print("\n");
        console_print("  xHCI xfer err:   "); console_print_dec(err); console_print("\n");
        console_print("  xHCI last cc/dci:"); console_print_dec(cc); console_print(" / "); console_print_dec(dci); console_print("\n");
    }

    {
        uint8_t b[8]; uint32_t len = 0, cnt = 0;
        xhci_debug_mouse(b, &len, &cnt);
        console_print("  USB mouse reports: "); console_print_dec(cnt);
        console_print("  last len: "); console_print_dec(len); console_print("  bytes:");
        for (int i = 0; i < 8; i++) { console_print(" "); console_print_hex(b[i]); }
        console_print("\n");
    }

    console_print("\n=== Mouse State ===\n");
    console_print("  Position: "); console_print_dec((uint32_t)mouse_get_x()); console_print(", "); console_print_dec((uint32_t)mouse_get_y()); console_print("\n");
    console_print("  Buttons:  0x"); console_print_hex(mouse_get_buttons()); console_print("\n");
    console_print("  Wheel:    "); console_print(mouse_has_wheel() ? "yes\n" : "no\n");
    console_print("  Modifiers: shift="); console_print(keyboard_shift_down() ? "down" : "up");
    console_print(" ctrl="); console_print(keyboard_ctrl_down() ? "down" : "up");
    console_print(" alt="); console_print(keyboard_alt_down() ? "down\n" : "up\n");
}
