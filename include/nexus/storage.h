#ifndef NEXUS_STORAGE_H
#define NEXUS_STORAGE_H

#include <stdint.h>

/*
 * Единый блочный интерфейс хранения данных NexusOS (секторы по 512 байт).
 * На x86_64 реализуется через AHCI/NVMe, на Tensor GS101 (Pixel 6 Pro) — через UFS 3.1 HCI.
 */

int  storage_is_ready(void);
int  storage_read_sectors(uint64_t lba, uint32_t count, void *buf);
int  storage_write_sectors(uint64_t lba, uint32_t count, const void *buf);

#endif /* NEXUS_STORAGE_H */
