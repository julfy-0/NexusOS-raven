/*
 * storage_ufs.c — Блочный адаптер UFS 3.1 для VFS и парсера GPT NexusOS.
 *
 * Транслирует вызовы storage_read_sectors() / storage_write_sectors() (сектора по 512 байт)
 * в нативные команды UFS 3.1 HCI (LUN0, геометрия блока blksz = 4096 или 512 байт).
 */

#include "a64.h"
#include "storage.h"
#include "ufs_gs101.h"

#define UFS_STORAGE_LUN 0

static uint8_t g_sector_buf[4096] __attribute__((aligned(64)));

int storage_is_ready(void) {
    struct ufs_gs101_status st;
    ufs_gs101_get_status(&st);
    return st.ready && st.blksz > 0;
}

int storage_read_sectors(uint64_t lba_512, uint32_t count_512, void *buf) {
    struct ufs_gs101_status st;
    ufs_gs101_get_status(&st);
    if (!st.ready || !st.blksz) return 0;

    uint8_t *dst = (uint8_t *)buf;

    /* Режим 1: Нативная геометрия 512 байт */
    if (st.blksz == 512) {
        return (ufs_gs101_read(UFS_STORAGE_LUN, lba_512, count_512, buf) == 0);
    }

    /* Режим 2: Нативная геометрия 4096 байт (1 UFS блок = 8 секторов по 512 байт) */
    if (st.blksz == 4096) {
        uint32_t ratio = 4096 / 512; /* 8 */

        while (count_512 > 0) {
            uint64_t blk_4k = lba_512 / ratio;
            uint32_t sec_offset = (uint32_t)(lba_512 % ratio);
            uint32_t sec_available = ratio - sec_offset;
            uint32_t sec_to_copy = (count_512 < sec_available) ? count_512 : sec_available;

            /* Если требуется полный блок 4K без смещения и count >= 8 */
            if (sec_offset == 0 && count_512 >= ratio) {
                uint32_t full_blocks = count_512 / ratio;
                if (ufs_gs101_read(UFS_STORAGE_LUN, blk_4k, full_blocks, dst) != 0) {
                    return 0;
                }
                uint32_t read_sec = full_blocks * ratio;
                dst += (size_t)read_sec * 512;
                lba_512 += read_sec;
                count_512 -= read_sec;
                continue;
            }

            /* Читаем один блок 4K в буфер и копируем нужный срез секторов */
            if (ufs_gs101_read(UFS_STORAGE_LUN, blk_4k, 1, g_sector_buf) != 0) {
                return 0;
            }

            memcpy(dst, g_sector_buf + (sec_offset * 512), (size_t)sec_to_copy * 512);
            dst += (size_t)sec_to_copy * 512;
            lba_512 += sec_to_copy;
            count_512 -= sec_to_copy;
        }
        return 1;
    }

    return 0;
}

int storage_write_sectors(uint64_t lba_512, uint32_t count_512, const void *buf) {
    struct ufs_gs101_status st;
    ufs_gs101_get_status(&st);
    if (!st.ready || !st.blksz) return 0;

    const uint8_t *src = (const uint8_t *)buf;

    /* Режим 1: Нативная геометрия 512 байт */
    if (st.blksz == 512) {
        return (ufs_gs101_write(UFS_STORAGE_LUN, lba_512, count_512, buf) == 0);
    }

    /* Режим 2: Нативная геометрия 4096 байт */
    if (st.blksz == 4096) {
        uint32_t ratio = 4096 / 512;

        while (count_512 > 0) {
            uint64_t blk_4k = lba_512 / ratio;
            uint32_t sec_offset = (uint32_t)(lba_512 % ratio);
            uint32_t sec_available = ratio - sec_offset;
            uint32_t sec_to_copy = (count_512 < sec_available) ? count_512 : sec_available;

            /* Полный блок 4K без смещения */
            if (sec_offset == 0 && count_512 >= ratio) {
                uint32_t full_blocks = count_512 / ratio;
                if (ufs_gs101_write(UFS_STORAGE_LUN, blk_4k, full_blocks, src) != 0) {
                    return 0;
                }
                uint32_t written_sec = full_blocks * ratio;
                src += (size_t)written_sec * 512;
                lba_512 += written_sec;
                count_512 -= written_sec;
                continue;
            }

            /* Частичный блок: Read-Modify-Write */
            if (ufs_gs101_read(UFS_STORAGE_LUN, blk_4k, 1, g_sector_buf) != 0) {
                return 0;
            }
            memcpy(g_sector_buf + (sec_offset * 512), src, (size_t)sec_to_copy * 512);
            if (ufs_gs101_write(UFS_STORAGE_LUN, blk_4k, 1, g_sector_buf) != 0) {
                return 0;
            }

            src += (size_t)sec_to_copy * 512;
            lba_512 += sec_to_copy;
            count_512 -= sec_to_copy;
        }
        return 1;
    }

    return 0;
}
