#ifndef NEXUSOS_REBOOT_H
#define NEXUSOS_REBOOT_H

/* Resets the machine: ACPI reset register, 8042, port 0xCF9, then triple
 * fault. Does not return. */
void reboot_run(void);

#endif
