#ifndef NEXUS_UFS_GS101_H
#define NEXUS_UFS_GS101_H
#include <stdint.h>
#include "nexus_version.h"
#include "usb_msc.h"

#define UFS_GS101_VERSION NEXUS_VERSION_STRING

struct ufs_gs101_status {
    uint8_t     ready, lun;
    uint8_t     lun_present[8];
    uint8_t     sense_key, asc, ascq;
    uint32_t    hci_ver, cap;
    uint32_t    blksz;            /* выбранный LUN */
    uint64_t    blocks;           /* выбранный LUN, в блоках blksz */
    const char *last_error;
};

int  ufs_gs101_init(void);                                            /* 0 = ок */
int  ufs_gs101_read (uint8_t lun, uint64_t lba, uint32_t n, void *buf);  /* блоки устройства */
int  ufs_gs101_write(uint8_t lun, uint64_t lba, uint32_t n, const void *buf);
int  ufs_gs101_flush(uint8_t lun);
void ufs_gs101_get_status(struct ufs_gs101_status *o);

/* Адаптер для usb_msc. Несуществующий LUN заменяется первым найденным. */
const struct usb_msc_backend *ufs_gs101_msc_backend(uint8_t lun, int writable);

#endif