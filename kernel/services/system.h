#ifndef NEXUSOS_SYSTEM_H
#define NEXUSOS_SYSTEM_H

#include "state.h"

void nexus_system_enter_application(void);
void nexus_system_enter_desktop(void);
void nexus_system_request_shutdown(void);
void nexus_system_request_reboot(void);

#endif
