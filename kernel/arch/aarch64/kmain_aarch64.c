#include "a64.h"
#include "wdt.h"
#include "gic_v3.h"
#include "touch_raven.h"
#include "btn_power.h"
#include "usb_gs101.h"
#include "psci_diag.h"
#include "gs101_disp.h"
#include "disp_draw.h"
#include "disp_ui.h"
#include "disp_boot_screen.h"
#include "disp_osk.h"
#include "usb_msc.h"
#include "phy_usb_gs101.h"
#include "ufs_gs101.h"
#include "storage.h"
#include "gpt.h"


/* 1 = USB отдаёт UFS как mass storage; 0 = старый режим usb_gs101 */
#ifndef USB_MODE_MSC
#define USB_MODE_MSC 1
#endif

void kmain_aarch64(uint64_t dtb, uint64_t load_base) {
    uint64_t el;
    mmu_init(load_base);
    mmu_map_2m(dtb, MMU_ATTR_WB);
    mmu_map_2m(dtb + 0x200000ULL, MMU_ATTR_WB);
    __asm__ volatile("dsb ishst; tlbi vmalle1; dsb nsh; isb");

    ramoops_init();
    ramoops_puts("[nexus] NexusOS aarch64 / Pixel 6 Pro (gs101): stage 1 alive\n");

    /* Отключаем аппаратные watchdog-таймеры для предотвращения автоперезагрузки */
    wdt_disable();

    /* Инициализируем контроллер прерываний GICv3 */
    gic_v3_init();
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
    ramoops_puts("[nexus] CurrentEL="); ramoops_put_hex(el >> 2);
    ramoops_puts(" load="); ramoops_put_hex(load_base);
    ramoops_puts(" dtb=");  ramoops_put_hex(dtb);
    ramoops_puts(" cntfrq="); ramoops_put_hex(timer_freq());
    ramoops_puts("\n");

    uint64_t mb = 0, ms = 0;
    int fdt_rc = fdt_first_memory((const void *)(uintptr_t)dtb, &mb, &ms);
    if (fdt_rc == 0) {
        ramoops_puts("[nexus] dram0 base="); ramoops_put_hex(mb);
        ramoops_puts(" size="); ramoops_put_hex(ms); ramoops_puts("\n");
    } else {
        ramoops_puts("[nexus] fdt: memory node not found\n");
    }

    /* Диагностика PSCI/SMCCC (только лог в ramoops) */
    psci_diag();

    /* Диагностика (только чтение): reboot-cmd (+0x810) и dump-gpr (+0x808) из узла exynos-reboot,
     * PS_HOLD (+0x3E9C). Для поиска cookie: Android -> "adb reboot bootloader" -> fastboot boot. */
    ramoops_puts("[pmu] reboot-cmd@810="); ramoops_put_hex(rd32(0x17460000ULL + 0x810));
    ramoops_puts(" dump-gpr@808="); ramoops_put_hex(rd32(0x17460000ULL + 0x808));
    ramoops_puts(" pshold@3e9c="); ramoops_put_hex(rd32(0x17460000ULL + 0x3E9C));
    ramoops_puts("\n");

    /* Ранние службы: дисплея ещё нет, строки копятся и покажутся при старте консоли */
    boot_svc("MMU / address space service", NEXUS_SERVICE_VERSION, 1);
    boot_svc("ramoops console service",     NEXUS_SERVICE_VERSION, 1);
    boot_svc("watchdog service (HW wdt off)", NEXUS_SERVICE_VERSION, 1);
    boot_svc("interrupt controller (ARM GICv3)", "1.0", 1);
    boot_svc("FDT parser service",          NEXUS_SERVICE_VERSION, fdt_rc == 0);

    struct disp_state ds;
    int disp_ok = 0;
    ramoops_puts("[nexus] display probe\n");
    disp_probe(&ds);
    if (disp_find_channel(&ds) == 0) {
        ramoops_puts("[disp] active ch="); ramoops_put_hex((uint64_t)ds.ch);
        ramoops_puts(" win="); ramoops_put_hex((uint64_t)ds.win);
        ramoops_puts(" fb="); ramoops_put_hex(ds.fb_phys);
        ramoops_puts(" w="); ramoops_put_hex(ds.width);
        ramoops_puts(" h="); ramoops_put_hex(ds.height);
        ramoops_puts(" pitch="); ramoops_put_hex(ds.pitch);
        ramoops_puts(" fmt="); ramoops_put_hex(ds.fmt);
        ramoops_puts(" sysmmu_on="); ramoops_put_hex(ds.sysmmu_on);
        ramoops_puts("\n");

        if (ds.sysmmu_on) {
            ramoops_puts("[disp] SysMMU DPU включен: адрес буфера = IOVA, прямое рисование пропущено\n");
        } else if (disp_map_fb(&ds) == 0) {
            disp_ok = 1;
            exc_set_display(&ds);
            boot_console_start(&ds);
            boot_svc("gpu driver (gs101 DPU/DECON)", GS101_DISP_VERSION, 1);
            boot_note_dec2("  display ", ds.width, " x ", ds.height);
        }
    } else {
        ramoops_puts("[disp] active channel not found\n");
    }

    /* Драйверы: строка на экране появляется после реальной инициализации,
     * статус — реальный результат (touch: DEVICE_ID прочитан; usb: GSNPSID валиден). */
    boot_note("  probing touch controller on SPI11 ...");
    int touch_rc = touch_raven_init();
    boot_svc("input: touch driver (sec_ts S6SY79x)", TOUCH_RAVEN_VERSION, touch_rc == 0);
    if (touch_rc != 0) {
        boot_note_cat("  ", touch_raven_reason());
        /* диагностика на экран (только чтение): USI11 sysreg, CMU_PERIC1 mux/div/QCH, SPI11 */
        boot_note_hex("  USI11 SW_CONF  ", rd32(0x10C21000ULL + 0x0C));
        boot_note_hex("  CMU mux  @640  ", rd32(0x10C00000ULL + 0x640));
        boot_note_hex("  CMU div  @180c ", rd32(0x10C00000ULL + 0x180c));
        boot_note_hex("  CMU qch  @302c ", rd32(0x10C00000ULL + 0x302c));
        boot_note_hex("  CMU gate @202c ", rd32(0x10C00000ULL + 0x202c));
        boot_note_hex("  CMU gate @2068 ", rd32(0x10C00000ULL + 0x2068));
        boot_note_hex("  SPI11 CH_CFG   ", rd32(0x10D40000ULL + 0x00));
        boot_note_hex("  SPI11 STATUS   ", rd32(0x10D40000ULL + 0x14));
        boot_note_hex("  SPI11 USI_CON  ", rd32(0x10D40000ULL + 0xC4));
    }

    disp_osk_init();
    boot_svc("on-screen keyboard service", NEXUS_SERVICE_VERSION, 1);

    btn_power_init();
    boot_svc("input: power/volume key driver", BTN_POWER_VERSION, 1);

#if USB_MODE_MSC
    {
        int urc = ufs_gs101_init();
        boot_svc("ufs storage (gs101 HCI)", UFS_GS101_VERSION, urc == 0);
        if (urc == 0) {
            struct ufs_gs101_status us;
            ufs_gs101_get_status(&us);
            boot_note_dec2("  LUN0 blocks ", (uint32_t)us.blocks, " x ", us.blksz);

            int nparts = gpt_scan();
            boot_svc("storage: GPT partition table", "1.0", nparts > 0);
            if (nparts > 0) {
                ramoops_puts("[gpt] detected partitions on UFS LUN0:\n");
                for (int i = 0; i < nparts; i++) {
                    const nexus_gpt_partition_t *p = gpt_partition(i);
                    ramoops_puts("  #"); ramoops_put_hex((uint64_t)p->index);
                    ramoops_puts(" "); ramoops_puts(p->name);
                    ramoops_puts(" LBA="); ramoops_put_hex(p->first_lba);
                    ramoops_puts(".."); ramoops_put_hex(p->last_lba);
                    ramoops_puts("\n");
                }
            }
        } else {
            struct ufs_gs101_status us;
            ufs_gs101_get_status(&us);
            boot_note_cat("  ", us.last_error);
            boot_note_hex("  UFS IS   ", rd32(0x14700000ULL + 0x20));
            boot_note_hex("  UFS HCS  ", rd32(0x14700000ULL + 0x30));
            boot_note_hex("  UFS NEXUS", rd32(0x14701100ULL + 0x40));
        }
        /* Инициализация USB PHY и снятие PMU-изоляции перед запуском DWC3 */
        phy_usb_gs101_init();
        boot_svc("usb phy (samsung usbdrd)", "1.0", 1);

        /* второй аргумент = writable; пока 0 — только чтение */
        int rc = usb_msc_init(urc == 0 ? ufs_gs101_msc_backend(0, 0) : 0);
        boot_svc("usb mass storage (DWC3 device)", USB_MSC_VERSION, rc == 0);
        if (rc != 0) {
            struct usb_msc_status ms_;
            usb_msc_get_status(&ms_);
            boot_note_cat("  ", ms_.last_error);
        }
    }
#else
    usb_gs101_init();
    {
        struct usb_gs101_status us;
        usb_gs101_get_status(&us);
        int usb_ok = (us.dwc3_version != 0 && us.dwc3_version != 0xFFFFFFFFU);
        boot_svc("usb driver (DWC3 xHCI)", USB_GS101_VERSION, usb_ok);
        boot_note_hex("  DWC3 GSNPSID=", us.dwc3_version);
        boot_note_cat("  Type-C cable: ", us.connected ? "connected" : "not connected");
    }
#endif

    boot_console_finish();

    /* 5 секунд на экране консоли загрузки перед запуском GUI */
    ramoops_puts("[nexus] holding boot console 5s before GUI\n");
    ramoops_flush();
    delay_sec(5);

    if (disp_ok) {
        /* Рабочий стол NexusOS */
        struct nexus_sysinfo info;
        info.current_el = el >> 2;
        info.load_base = load_base;
        info.dtb = dtb;
        info.timer_freq = timer_freq();
        info.dram_base = mb;
        info.dram_size = ms;

        disp_ui_draw(&ds, &info);
        disp_present(&ds);
        ramoops_puts("[disp] NexusOS UI rendered and presented successfully\n");
    }

    ramoops_puts("[nexus] entering operational main loop\n");
    ramoops_flush();

    /* Рабочий цикл ядра: частота опроса 50 Гц (20 мс) для плавного отклика тачскрина */
    uint32_t uptime_sec = 0;
    uint32_t tick_50hz = 0;
    while (1) {
        /* Проверяем события сенсорного экрана и интерактивный ввод */
                struct touch_state ts;
        if (touch_raven_poll(&ts)) {
            if (disp_ok) {
                touch_raven_render_feedback(&ds, &ts);
                if (ts.num_points > 0 && ts.points[0].pressed) {
                    int tx = ts.points[0].x, ty = ts.points[0].y;
                    if (ds.width  != 1440) tx = (int)((uint64_t)tx * ds.width  / 1440);
                    if (ds.height != 3120) ty = (int)((uint64_t)ty * ds.height / 3120);
                    disp_ui_handle_touch(&ds, tx, ty, 1);
                }
                disp_present(&ds);
            } else {
                ramoops_puts("[touch] ev x="); ramoops_put_hex(ts.points[0].x);
                ramoops_puts(" y="); ramoops_put_hex(ts.points[0].y);
                ramoops_puts(" p="); ramoops_put_hex(ts.points[0].pressed);
                ramoops_puts("\n");
            }
        }

        /* Проверяем кнопки питания/громкости:
         *   POWER 5 сек       → выключение (PSCI SYSTEM_OFF)
         *   POWER + VOL_DOWN  → перезагрузка в fastboot */
        if (disp_ok) {
            btn_power_check(&ds);
        } else {
            btn_power_check(0);
        }

        /* Опрашиваем состояние USB порта (DWC3 / Type-C) */
#if USB_MODE_MSC
        usb_msc_service(20);
#else
        usb_gs101_poll();
        delay_ms(20);
#endif
        tick_50hz++;

        /* Раз в секунду (каждые 50 тиков по 20 мс) */
        if (tick_50hz >= 50) {
            tick_50hz = 0;
            uptime_sec++;

            /* Подпитываем watchdog, чтобы не сработал аппаратный сброс */
            wdt_pet();

            if (disp_ok) {
                disp_ui_update_heartbeat(&ds, uptime_sec);
                disp_present(&ds);
            }
            if ((uptime_sec % 30) == 0) {
                ramoops_puts("[nexus] alive uptime=");
                ramoops_put_hex((uint64_t)uptime_sec);
                ramoops_puts("s\n");
                ramoops_flush();
            }
        }
    }
}