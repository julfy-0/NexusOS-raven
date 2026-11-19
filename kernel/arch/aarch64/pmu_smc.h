#ifndef NEXUS_PMU_SMC_H
#define NEXUS_PMU_SMC_H
#include <stdint.h>

/* Запись в защищённые регистры PMU gs101 через SMC (EL3).
 * ИСТОЧНИК: mainline Linux drivers/soc/samsung/exynos-pmu.c (патч Peter Griffin, 2024),
 *   TENSOR_SMC_PMU_SEC_REG = 0x82000504, op: READ=0, WRITE=1, RMW=2.
 * НЕ из ~/ref (репозитория Google под рукой нет). На железе не проверено. */

/* Базовый физический адрес PMU (gs101.dtsi: system-controller@17460000, size 0x10000). */
#define PMU_SMC_BASE 0x17460000ULL

/* Прямая запись: *(PMU+off) = val. Возвращает x0 из SMC (0 = успех, иначе ошибка). */
uint64_t pmu_smc_write(uint32_t off, uint32_t val);

/* Read-modify-write: reg = (reg & ~mask) | (val & mask), выполняет EL3. */
uint64_t pmu_smc_rmw(uint32_t off, uint32_t mask, uint32_t val);

/* Атомарные set/clear одного бита для регистров PMU_ALIVE (off <= 0x3FFF).
 * В mainline: offset |= 0xC000 (set) / 0x8000 (clear), в value передаётся НОМЕР бита. */
uint64_t pmu_smc_set_bit(uint32_t off, uint32_t bit);
uint64_t pmu_smc_clear_bit(uint32_t off, uint32_t bit);

#endif
