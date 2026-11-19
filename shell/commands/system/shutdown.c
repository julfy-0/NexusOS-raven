#include "shutdown.h"
#include "console.h"
#include "power.h"

void shutdown_run(void) {
    console_print("Shutting down...\n");
    nexus_power_shutdown();
}
