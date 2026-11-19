#ifndef NEXUSOS_POWER_H
#define NEXUSOS_POWER_H

/* Parses the ACPI tables handed over by the UEFI loader (FADT -> PM1 control
 * blocks, reset register, DSDT \_S5_). Safe to call more than once. */
int nexus_power_init(void);

/* Both functions really power off / reset the machine and do not return. */
void nexus_power_shutdown(void) __attribute__((noreturn));
void nexus_power_reboot(void)   __attribute__((noreturn));

/* 1 if ACPI S5 data was found (real hardware shutdown is possible). */
int nexus_power_acpi_ready(void);

#endif
