# NexusOS -> Pixel 6 Pro (gs101 / raven)

Сборка: `make -f Makefile.pixel6pro` -> `build/pixel6pro/boot.img` (нужен `aarch64-linux-gnu-gcc`).

## Что сделано (этап 1)
- `kernel/arch/aarch64/entry.S` — заголовок arm64 Image, EL2->EL1, самоперемещение, bss, векторы.
- `mmu_init.c`, `mmu_map_2m.c` — MMU: 0..1 ГиБ MMIO (Device), окно ядра (Normal WB), ramoops (Normal NC).
- `ramoops_*.c` — лог в консольный буфер ramoops 0xFD3FF000 (формат из fs/pstore/ram_core.c).
- `fdt_first_memory.c` — первый узел memory из DTB, который передаёт ABL в x0.
- `psci_system_reset.c` — перезагрузка по PSCI через smc.
- `tools/mkbootimg_pixel.py` — boot.img v4 только с ядром; DTB и cmdline берутся из штатного vendor_boot.

## Факты из исходников ядра (arch/arm64/boot/dts/google)
- GICv3: GICD 0x10400000, GICR 0x10440000 (gs101.dtsi)
- Таймер: arch timer, clock-frequency 24576000
- UART: 0x10A00000 (exynos4210), нужен debug-кабель
- UFS 0x14700000; USB DWC3 0x11110000
- DECON0 0x1C300000, DSIM0 0x1C2C0000, панель Raven: samsung,s6e3hc3
- ramoops 0xFD3FF000, 4 МиБ: console 2 МиБ + pmsg 2 МиБ
- DRAM начинается с 0x80000000 (узла memory в DTS нет, его заполняет загрузчик)

## Блокеры
1. Дисплей: драйвера Samsung DPU/DECON (описание регистров) в этом дереве нет, каталога drivers/gpu/drm/samsung нет. Он лежит в отдельном репозитории google-modules/display. Без него экран не написать, а без экрана GUI не запустится.
2. Ввод: тачскрин на SPI (`spitouch`), кнопки через PMIC.
3. Хранилище: UFS вместо AHCI/NVMe. USB: DWC3 вместо xHCI/EHCI.
4. Не переносится как есть: UEFI-загрузчик, GDT/IDT/PIC/PIT/TSC, PCI, e1000, GPU-драйверы Intel/AMD/NVIDIA. Переносимо после замены arch-слоя: kernel/core, kernel/mm, kernel/fs, gui, shell.

## Статус проверки
- Проверено в QEMU virt (cortex-a76, EL1 и EL2): старт, самоперемещение, MMU, запись лога, чтение FDT.
- Не проверено на телефоне: адрес загрузки ABL, сохранность ramoops после reset, принятие boot.img v4 без ramdisk, smc-reset.

## Запуск на телефоне
Только с разблокированным загрузчиком. Сначала сохрани штатный boot: `fastboot fetch boot boot_stock.img` (если не поддерживается — достань из полной прошивки).
`fastboot boot build/pixel6pro/boot.img`, затем в Android: `adb shell su -c "cat /sys/fs/pstore/console-ramoops-0"`.

## Этап 3: стабильность и ввод (WDT и тачскрин)
- `kernel/arch/aarch64/wdt.c`, `wdt.h`: отключение аппаратных watchdog-таймеров Tensor GS101 (Cluster 0 `0x10060000`, Cluster 1 `0x10070000`, маскирование PMU `0x17460000`), а также регулярный keepalive `wdt_pet()` в основном цикле для предотвращения перезагрузки.
- `kernel/arch/aarch64/exc_report.c`: замена безусловного `psci_system_reset()` на безопасный цикл ожидания `wfe` с выводом трассировки в ramoops, чтобы сбои не приводили к немедленному ребуту.
- `kernel/arch/aarch64/touch_raven.c`, `touch_raven.h`: полноценный аппаратный опрос контроллера тачскрина Samsung SEC_TS (S6SY791) через SPI_11 / USI11 (`0x10D40000`) в режиме Mode 3 (CPOL=1, CPHA=1), разбор пакетов `SEC_TS_READ_ONE_EVENT` (код 0x60, сетка 1440x3120) и визуальный отклик касаний на AMOLED-панели.
- `kernel/drivers/gpu/gs101/disp_ui.c`, `disp_ui.h`: масштабирование под 1440x3120 AMOLED (масштаб шрифта 3, высота статус-бара 80px, карточки 280px, 4 интерактивные вкладки App Dock с откликом на касания, индикатор кликов `TOUCH CLICKS`).
- `kernel/drivers/gpu/gs101/disp_osk.c`, `disp_osk.h`: крупная 5-рядная экранная клавиатура с высотой клавиш 145px (удобно для пальцев), масштаб шрифта 3, командная строка терминала.
- `kernel/arch/aarch64/touch_raven.c`: аппаратная инициализация пинов PERIC1 `GPP26` (TS_RESET_L, TS_SPI_SEL), импульс сброса контроллера S6SY791, настройка функции SPI `GPP23` и генерация тактов через `REG_SPI_PACKET_CNT`.

Файлы: `kernel/drivers/gpu/gs101/*` (`disp_probe`, `disp_find_channel`, `disp_map_fb`, `disp_fill_bars`, `disp_present`).
Источник регистров: google-modules/display, ветка android-gs-raviole-5.10-android13, `samsung/cal_9845/regs-decon.h`, `regs-dpp.h` (GPL-2.0).
Логика: читаем DECON0/WINCON/IDMA/SysMMU, находим канал, который сейчас сканируется (теневой адрес +0x400),
если SysMMU DPU выключены, то рисуем 8 цветных полос прямо в буфер ABL и запрашиваем кадр (shadow update + trigger).
Весь вывод проб идёт в ramoops (строка пишется до чтения регистра: при зависании видно, на каком регистре).
Не проверено на телефоне. Полная инициализация DSIM/DSC/панели не сделана.
Чтение лога: `adb shell su -c "cat /sys/fs/pstore/console-ramoops-0"`.

## Этап 4: Интеграция стоковых драйверов GS101 (Завершено)
- `vendor/stock-drivers/`: распакованы эталонные стоковые драйверы Linux 5.10 (549 файлов: DTS, UFS CAL, DWC3/PHY, SPI, Pinctrl, WDT).
- `kernel/arch/aarch64/gs101_gpio.c`, `gs101_gpio.h`: единый драйвер Pinctrl/GPIO для всех банков (PERIC0/1, ALIVE, FAR_ALIVE, HSI).
- `kernel/arch/aarch64/spi_gs101.c`, `spi_gs101.h`: абстракция контроллера шины Samsung S3C64xx (USI11) с управлением тактированием CMU и мультиплексированием SYSREG.
- `kernel/drivers/input/gs101/touch_raven.c` & `kernel/arch/aarch64/btn_power.c`: полный рефакторинг тачскрина и кнопок питания/громкости с переходом на чистые вызовы `gs101_gpio_*` и `spi_gs101_*`.
- `kernel/arch/aarch64/storage_ufs.c` & `include/nexus/storage.h`: блочный адаптер UFS 3.1, транслирующий 512-байтовые LBA в 4K блоки UFS.
- `kernel/fs/gpt.c`: включен парсер GPT разделов. При старте ядра сканируются и выводятся все аппаратные разделы Pixel 6 Pro (`boot_a`, `system_a`, `userdata`).
- `kernel/arch/aarch64/phy_usb_gs101.c`, `phy_usb_gs101.h`: драйвер Samsung USB 3.0 DRD PHY, управление изоляцией линий через PMU (`0x17463EB0`/`0x17463EB4`) и калибровка параметров High-Speed.
- `kernel/arch/aarch64/gic_v3.c`, `gic_v3.h`: аппаратный контроллер прерываний ARM GICv3 (Distributor `0x10400000`, Redistributor CPU0 `0x10440000`, CPU Interface ICC через системные регистры AArch64).
- `kernel/drivers/gpu/gs101/gs101_disp.h`: интеграция параметров панели Raven (`samsung,s6e3hc3`, 1440x3120, профили 60/120 Гц) и базовых адресов DPU/DSIM/DPP.
- `Makefile.pixel6pro`: сборка обновлена, компиляция ядра завершается со статусом OK (`boot.img` 647 КБ).
