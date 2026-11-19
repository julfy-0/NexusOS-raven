# Руководство по переносу стоковых драйверов в NexusOS (Pixel 6 Pro / Tensor GS101)

Данный документ описывает регламент и пошаговый процесс адаптации стоковых драйверов Google/Samsung (`stock-drivers.tgz`, ядро Linux 5.10 `android-gs-raviole`) в микроядро/монолит NexusOS AArch64.

---

## 1. Архитектурные принципы и особенности платформы

### 1.1. Контекст запуска на Pixel 6 Pro ("raven")
* **Загрузчик (ABL)**: Загружает ядро в режиме AArch64 EL2/EL1 из `boot.img` (v4). В регистр `x0` передаётся физический адрес FDT (Flattened Device Tree), в `x1`–`x3` — зарезервированные параметры.
* **Среда выполнения**: Freestanding (`-ffreestanding -nostdlib -fPIE -mgeneral-regs-only`). Стандартной библиотеки libc и подсистем ядра Linux нет.
* **Главный канал отладки**: Ramoops pstore буфер в оперативной памяти по адресу `0xFD3FF000` (размер 4 МиБ: 2 МиБ консоль + 2 МиБ pmsg). Читается после ребута в Android:
  ```bash
  adb shell su -c "cat /sys/fs/pstore/console-ramoops-0"
  ```
* **Питание и тактирование**: Блоки периферии (DPU, USB PHY, UFS, SPI) завязаны на контроллер питания PMU (`0x17460000`) и подсистему ACPM (Active Core Power Management). **Обращение к регистрам обесточенного блока вызывает аппаратный SError (Synchronous External Abort)**.

---

## 2. Пошаговый регламент переноса

```
Архив stock-drivers.tgz
   │
   ├──► 1. Распаковка в vendor/stock-drivers/ [ВЫПОЛНЕНО]
   │
   ├──► 2. Выделение заголовочных файлов и констант [ВЫПОЛНЕНО]
   │
   ├──► 3. Реализация платформенного GPIO / Pinctrl (gs101_gpio.c/h) [ВЫПОЛНЕНО]
   │
   ├──► 4. Абстракция шины USI / SPI (spi_gs101.c/h) [ВЫПОЛНЕНО]
   │
   ├──► 5. Рефакторинг тачскрина S6SY79x и кнопок (btn_power.c) [ВЫПОЛНЕНО]
   │
   ├──► 6. Блочный адаптер UFS 3.1 (storage_ufs.c) и сканер GPT (gpt.c) [ВЫПОЛНЕНО]
   │
   ├──► 7. Управление USBDRD PHY и снятие PMU-изоляции (phy_usb_gs101.c/h) [ВЫПОЛНЕНО]
   │
   ├──► 8. Контроллер прерываний ARM GICv3 (gic_v3.c/h) [ВЫПОЛНЕНО]
   │
   └──► 9. Интеграция параметров дисплея DPU/DECON (gs101_disp.h) [ВЫПОЛНЕНО]
```

---

### Шаг 1. Распаковка архива и организация каталогов

1. Создайте изолированный каталог вендорных исходников:
   ```bash
   mkdir -p vendor/stock-drivers
   mkdir -p include/nexus/arch/aarch64/gs101
   
   # Распаковка архива stock-drivers.tgz
   tar -xzf stock-drivers.tgz -C vendor/stock-drivers
   ```

2. Скопируйте критические заголовочные файлы:
   * Из `vendor/stock-drivers/include/soc/google/` → в `include/nexus/arch/aarch64/gs101/`
   * Из `vendor/stock-drivers/include/dt-bindings/` → в `include/nexus/arch/aarch64/gs101/dt-bindings/`

---

### Шаг 2. Реализация Pinctrl & GPIO (`gs101_gpio`)

В стоковом ядре описание банков пинов находится в `drivers/pinctrl/samsung/pinctrl-gs.c`.

#### 2.1. Интерфейс `kernel/arch/aarch64/gs101_gpio.h`
```c
#ifndef GS101_GPIO_H
#define GS101_GPIO_H

#include "a64.h"

/* Базовые адреса контроллеров Pinctrl GS101 */
#define GS101_PINCTRL_PERIC0   0x10840000ULL
#define GS101_PINCTRL_PERIC1   0x10C40000ULL
#define GS101_PINCTRL_ALIVE    0x174D0000ULL
#define GS101_PINCTRL_HSI      0x14440000ULL

/* Смещения регистров внутри банка пинов */
#define PIN_REG_CON            0x00  /* Режим: 0=Input, 1=Output, 2..F=Функции шин */
#define PIN_REG_DAT            0x04  /* Данные (чтение/запись уровня) */
#define PIN_REG_PUD            0x08  /* Подтяжка: 0=Off, 1=Down, 3=Up */
#define PIN_REG_DRV            0x0C  /* Сила тока: 1X, 2X, 3X, 4X */

enum gpio_pull {
    GPIO_PULL_NONE = 0,
    GPIO_PULL_DOWN = 1,
    GPIO_PULL_UP   = 3,
};

/* API настройки */
void gs101_gpio_cfg_pin(uint64_t bank_base, uint32_t pin, uint32_t func);
void gs101_gpio_set_pull(uint64_t bank_base, uint32_t pin, enum gpio_pull pull);
void gs101_gpio_set_val(uint64_t bank_base, uint32_t pin, int val);
int  gs101_gpio_get_val(uint64_t bank_base, uint32_t pin);

#endif
```

#### 2.2. Реализация `kernel/arch/aarch64/gs101_gpio.c`
```c
#include "gs101_gpio.h"
#include "gs101_disp.h" /* rd32 / wr32 */

void gs101_gpio_cfg_pin(uint64_t bank_base, uint32_t pin, uint32_t func) {
    uint32_t shift = pin * 4;
    uint32_t mask = 0xFU << shift;
    uint32_t val = rd32(bank_base + PIN_REG_CON);
    val = (val & ~mask) | ((func & 0xFU) << shift);
    wr32(bank_base + PIN_REG_CON, val);
}

void gs101_gpio_set_pull(uint64_t bank_base, uint32_t pin, enum gpio_pull pull) {
    uint32_t shift = pin * 4;
    uint32_t mask = 0xFU << shift;
    uint32_t val = rd32(bank_base + PIN_REG_PUD);
    val = (val & ~mask) | (((uint32_t)pull & 0xFU) << shift);
    wr32(bank_base + PIN_REG_PUD, val);
}

void gs101_gpio_set_val(uint64_t bank_base, uint32_t pin, int val) {
    uint32_t reg = rd32(bank_base + PIN_REG_DAT);
    if (val)
        reg |= (1U << pin);
    else
        reg &= ~(1U << pin);
    wr32(bank_base + PIN_REG_DAT, reg);
}

int gs101_gpio_get_val(uint64_t bank_base, uint32_t pin) {
    return (rd32(bank_base + PIN_REG_DAT) >> pin) & 1U;
}
```

---

### Шаг 3. Драйвер шины USI / SPI и рефакторинг тачскрина

В стоковом ядре логика работы с Samsung USI SPI содержится в `drivers/spi/spi-s3c64xx.c`.

1. **Создание драйвера шины `kernel/arch/aarch64/spi_s3c64xx.c`**:
   - Извлечь смещения регистров:
     * `S3C64XX_SPI_CH_CFG` (`0x00`)
     * `S3C64XX_SPI_CLK_CFG` (`0x04`)
     * `S3C64XX_SPI_MODE_CFG` (`0x08`)
     * `S3C64XX_SPI_SLAVE_SEL` (`0x0C`)
     * `S3C64XX_SPI_STATUS` (`0x14`)
     * `S3C64XX_SPI_TX_DATA` (`0x18`)
     * `S3C64XX_SPI_RX_DATA` (`0x20`)
     * `S3C64XX_SPI_PACKET_CNT` (`0x24`)
   - Реализовать функцию полнодуплексной передачи `spi_transfer(base, tx_buf, rx_buf, len)`.
2. **Очистка `touch_raven.c`**:
   - Заменить прямое дёргание регистров `0x10C400C0` на `gs101_gpio_set_val(GS101_PINCTRL_PERIC1 + 0xC0, 0, 1)` (для `TS_RESET_L` и `TS_SPI_SEL`).
   - Перевести обмен пакетами тач-событий на `spi_transfer()`.

---

### Шаг 4. UFS 3.1: Калибровка PHY и подключение к блочному стеку

В стоковом драйвере `drivers/scsi/ufs/gs101/ufs-cal-if.c` уже предусмотрен макрос `__UFS_CAL_LK__` для автономной работы.

1. **Интеграция калибровки PHY**:
   - Перенести структуру `struct ufs_cal_phy_cfg` и таблицу настроек M-PHY (`cal_table`) из `ufs-cal-if.c`.
   - В `ufs_gs101.c` перед отправкой первых NOP/Query команд выполнять последовательность:
     * Включение аналоговых клоков UFS.
     * Запись настроек эквалайзера и CDR линий TX/RX.
     * Запуск софт-сброса контроллера (`HCE.HCE = 1`).
2. **Подключение к VFS**:
   - Оформить `ufs_gs101` как блочный интерфейс:
     ```c
     struct block_device {
         const char *name;
         uint64_t total_blocks;
         uint32_t block_size;
         int (*read_blocks)(uint64_t lba, uint32_t count, void *buf);
         int (*write_blocks)(uint64_t lba, uint32_t count, const void *buf);
     };
     ```
   - Зарегистрировать в `kernel/fs/gpt.c` для поиска разделов (`system_a`, `boot_a`, `userdata`).

---

### Шаг 5. USB DWC3 & PHY: Реализация CDC-ACM Serial

Для отладки ядра без аппаратного debug-кабеля (UART) USB-порт настраивается как виртуальный последовательный порт (CDC-ACM).

1. **Инициализация PHY из `phy-exynos-usbdrd.c`**:
   - Включение питания USB3 DRD блока через PMU.
   - Сброс и калибровка UTMI+ / PIPE3 интерфейса PHY.
2. **Настройка конечных точек (Endpoints) DWC3**:
   - `EP0` (Control): стандартные запросы энумерации USB (`GET_DESCRIPTOR`, `SET_ADDRESS`, `SET_CONFIGURATION`).
   - `EP1 IN` (Bulk): передача строк лога ядра в терминал ПК (`stdout`).
   - `EP2 OUT` (Bulk): приём ввода с клавиатуры ПК в интерактивный шелл NexusOS (`stdin`).
   - Дескрипторы устройства: VID/PID Google (`18d1:4ee7` — Pixel USB debug).

---

### Шаг 6. Дисплейная подсистема (DPU / DECON / DPP)

На основе `gs101-drm-dpu.dtsi` и `gs101-raven-display.dtsi`:
1. **Базовый режим**: Продолжать сканировать канал DECON0 (`0x1C300000`), инициализированный ABL, получая физический адрес текущего кадрового буфера.
2. **Собственный видеорежим**:
   - Настроить тайминги панели S6E3HC3: разрешение `1440 x 3120`, частота 60 Гц или 120 Гц.
   - Подключить обработку прерывания `IRQ_DECON0_FRAME_DONE` для синхронного переключения экранных буферов (исключение артефактов и тиринга).

---

## 3. Сборка, прошивка и валидация

### 3.1. Обновление Makefile
В файле `Makefile.pixel6pro` добавить новые модули:
```makefile
C_SRCS += kernel/arch/aarch64/gs101_gpio.c \
          kernel/arch/aarch64/spi_s3c64xx.c
```

### 3.2. Компиляция и создание образа
```bash
# Из окружения WSL/Linux:
make -f Makefile.pixel6pro clean
make -f Makefile.pixel6pro -j$(nproc)
```
Результат сборки: `build/pixel6pro/boot.img`.

### 3.3. Загрузка на смартфон
> Смартфон должен иметь разблокированный загрузчик (`fastboot flashing unlock`).
> **Никогда** не прошивайте ядро намертво в раздел `boot` (`fastboot flash boot`) на этапе разработки! Используйте только временную загрузку в RAM.

1. Переведите телефон в режим Fastboot:
   Зажмите клавиши `Power` + `Volume Down`.
2. Запустите образ:
   ```bash
   fastboot boot build/pixel6pro/boot.img
   ```
3. Снимите диагностический лог:
   Перезагрузитесь в штатный Android или Recovery и выполните:
   ```bash
   adb shell su -c "cat /sys/fs/pstore/console-ramoops-0" > boot_debug.log
   ```

---

## 4. Диагностика частых проблем (Troubleshooting)

| Симптом | Причина | Способ устранения |
|---|---|---|
| **Немедленный ребут через 2–5 сек** | Сработал аппаратный WDT (Watchdog) кластера CPU | Проверить вызов `wdt_disable()` в начале `kmain_aarch64` и убедиться, что `wdt_pet()` вызывается регулярно. |
| **Черный экран, подсветка горит** | SysMMU DPU включен загрузчиком (требуются виртуальные адреса IOVA) | Читать флаг `sysmmu_on` в `disp_probe()`. При активном SysMMU рисовать только через настроенные ABL дескрипторы DMA. |
| **Ядро зависает на чтении регистра** | Обесточен домен питания или отключен тактовый генератор (Clock Gate) | Проверить регистры PMU (`0x17460000 + 0x3E9C`), подать питание на блок через PMU/ACPM до чтения регистров. |
| **Тачскрин не отвечает** | Не снят ресет `TS_RESET_L` или пин выбора шины `TS_SPI_SEL` в режиме AOC | Проверить уровни на `GPP26[0]` (должна быть 1) и `GPP26[1]` (должен быть 0). |
