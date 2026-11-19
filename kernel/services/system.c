#include "system.h"

void nexus_system_enter_application(void) { nexus_system_set_state(NEXUS_SYSTEM_APPLICATION); }
void nexus_system_enter_desktop(void) { nexus_system_set_state(NEXUS_SYSTEM_DESKTOP); }
void nexus_system_request_shutdown(void) { nexus_system_set_state(NEXUS_SYSTEM_SHUTDOWN); }
void nexus_system_request_reboot(void) { nexus_system_set_state(NEXUS_SYSTEM_REBOOT); }
