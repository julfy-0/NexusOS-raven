#ifndef NEXUS_USB_MSC_H
#define NEXUS_USB_MSC_H
#include <stdint.h>
#include "nexus_version.h"

#define USB_MSC_VERSION NEXUS_VERSION_STRING

struct usb_msc_backend {
    uint32_t (*block_count)(void);                                  /* секторов по 512 Б */
    int      (*read)(uint32_t lba, uint32_t n, void *buf);          /* 0 = ок */
    int      (*write)(uint32_t lba, uint32_t n, const void *buf);   /* 0 = ок */
    int      (*flush)(void);                                        /* может быть 0 */
    int      writable;
};

struct usb_msc_status {
    uint8_t     running, connected, configured, speed;
    uint32_t    cmds, read_blocks, write_blocks;
    const char *last_error;
};

int  usb_msc_init(const struct usb_msc_backend *b);   /* b == 0 -> RAM-диск */
void usb_msc_service(uint32_t ms);                    /* вместо delay_ms в главном цикле */
void usb_msc_get_status(struct usb_msc_status *o);

#endif