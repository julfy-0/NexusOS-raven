#ifndef NEXUSOS_SHUTDOWN_H
#define NEXUSOS_SHUTDOWN_H
/* Powers the machine off through ACPI S5 (real hardware), with fallbacks for
 * QEMU/Bochs/VirtualBox. Does not return. */
void shutdown_run(void);
#endif
