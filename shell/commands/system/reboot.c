#include "reboot.h"
#include "console.h"
#include "power.h"

void reboot_run(void) {
    console_print("Rebooting...\n");
    nexus_power_reboot();
}
