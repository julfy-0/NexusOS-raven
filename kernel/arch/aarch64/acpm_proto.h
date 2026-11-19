#ifndef ACPM_PROTO_H
#define ACPM_PROTO_H
#include <stdint.h>
/* Только подтверждённое исходниками ~/gs. Транспорт (очереди) НЕ реализован:
   нет struct ipc_channel / acpm_framework. */
#define ACPM_MBOX_BASE      0x17610000UL   /* gs101.dtsi:487 */
#define ACPM_SRAM_BASE      0x02039000UL   /* gs101.dtsi:488 */
#define ACPM_SRAM_SIZE      0x40000UL
#define ACPM_INITDATA_OFF   0xA000UL       /* gs101.dtsi:490 */
#define ACPM_INTCR0         0x0024         /* acpm_ipc.h:96 */
#define ACPM_INTMR0         0x0028
#define ACPM_INTSR0         0x002C
#define ACPM_INTGR1         0x0040         /* doorbell к APM */
#define ACPM_SEQ_SHIFT      16             /* acpm_ipc_ctrl.h:31 */
#define ACPM_SEQ_MASK       0x3Fu
#define MFD_FUNC_READ       0
#define MFD_FUNC_WRITE      1
static inline uint32_t acpm_mfd_cmd0(uint16_t type, uint8_t reg, uint8_t ch) {
    return ((uint32_t)(type & 0xF) << 8) | reg | ((uint32_t)(ch & 0xF) << 12);
}
static inline uint32_t acpm_mfd_cmd1_read(void) { return MFD_FUNC_READ; }
static inline uint32_t acpm_mfd_cmd1_write(uint8_t v) { return MFD_FUNC_WRITE | ((uint32_t)v << 8); }
static inline uint8_t acpm_mfd_ret(uint32_t c1)  { return (c1 >> 24) & 0xFF; }
static inline uint8_t acpm_mfd_dest(uint32_t c1) { return (c1 >> 8) & 0xFF; }
static inline uint32_t acpm_set_seq(uint32_t c0, uint32_t s) { return c0 | ((s & ACPM_SEQ_MASK) << ACPM_SEQ_SHIFT); }
static inline uint32_t acpm_get_seq(uint32_t c0) { return (c0 >> ACPM_SEQ_SHIFT) & ACPM_SEQ_MASK; }
#define S2MPG10_PM_L25M_CTRL 0x5C          /* s2mpg10-register.h:108 */
#define S2MPG10_PM_L26M_CTRL 0x5D
#define S2MPG10_LDO_EN_BIT   (1u << 7)     /* s2mpg10-register.h:325 */
#endif
