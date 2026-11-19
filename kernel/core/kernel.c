/* NexusOS kernel — kmain.
 *
 * На входе: boot services уже мертвы, framebuffer доступен напрямую,
 * paging — тот, что оставила прошивка (identity-map). Своя MMU-настройка —
 * следующий милстоун. */
#include <stdint.h>
#include "boot_info.h"
#include "console.h"
#include "gdt.h"
#include "idt.h"
#include "paging.h"
#include "pmm.h"
#include "vmm.h"
#include "heap.h"
#include "pic.h"
#include "kstate.h"
#include "shell.h"
#include "mount.h"
#include "pit.h"
#include "pci.h"
#include "storage.h"
#include "fat32.h"
#include "keyboard.h"
#include "gui.h"
#include "usermode.h"
#include "process.h"
#include "syscall.h"
#include "xhci.h"
#include "gpu.h"
#include "usb.h"
#include "mouse.h"
#include "drivers/input/input.h"
#include "kernel_events.h"
#include "event_queue.h"
#include "scheduler.h"
#include "target.h"
#include "nexus_version.h"
#include "session.h"
#include "power.h"
#include "system_info.h"
#include "app_manager.h"
#include "package_manager.h"
#include "network.h"
#include "module.h"
#include "watchdog.h"
#include "heap.h"
#include "panic.h"

static volatile uint64_t g_scheduler_service_ticks;

static void scheduler_service_thread(void *arg) {
    (void)arg;
    for (;;) {
        if (kernel_events_wait(NEXUS_EVENT_KEYBOARD_SCANCODE)) {
            g_scheduler_service_ticks++;
        }
    }
}

/* boot_require — печатает статус компонента и останавливает систему при FAIL.
 * Используется для критических компонентов, без которых ОС не может работать.
 * Для некритических (optional) — используй console_component_status напрямую. */
static void boot_require(const char *name, const char *type,
                         const char *version, int ok,
                         const char *fail_reason) {
    console_component_status(name, type, version, ok);
    if (!ok) {
        if (fail_reason) {
            console_print("  Reason: ");
            console_print(fail_reason);
            console_print("\n");
        }
        critical_os_stop(name);
    }
}

void kmain(nexus_boot_info_t *boot_info) {
    __asm__ volatile ("cli" ::: "memory");

    if (boot_info == 0 || boot_info->magic != NEXUS_BOOT_MAGIC ||
        boot_info->fb.base == 0 || boot_info->fb.width == 0 ||
        boot_info->fb.height == 0 || boot_info->kernel_phys_end <= boot_info->kernel_phys_base ||
        boot_info->kernel_entry == 0) {
        for (;;) { __asm__ volatile ("cli; hlt"); }
    }

    console_init(&boot_info->fb);
    kstate_set_boot_info(boot_info);

    console_set_color(COLOR_CYAN, COLOR_BLACK);
    console_print("NexusOS " NEXUS_VERSION_DISPLAY "\n");
    console_set_color(COLOR_WHITE, COLOR_BLACK);
    console_print("================================\n\n");

    /* ── Критические сервисы ядра ─────────────────────────────────────── */
    gdt_init();
    boot_require("GDT", "service", "1.0", 1, NULL);

    idt_init();
    boot_require("IDT", "service", "1.0", 1, NULL);

    paging_init(boot_info);
    boot_require("kernel address space", "service", "1.0", 1, NULL);

    pmm_init(boot_info);
    boot_require("physical memory manager", "service", "1.0",
                 pmm_total_pages() != 0,
                 "no usable physical memory pages found");

    vmm_init();
    boot_require("virtual memory manager", "service", "1.0", 1, NULL);

    heap_init();
    boot_require("kernel heap", "service", "1.0",
                 heap_is_ready(),
                 "heap allocator failed to initialize");

    {
        void *heap_test = kmalloc(64);
        int allocator_ok = heap_test != NULL;
        if (allocator_ok) kfree(heap_test);
        boot_require("kernel allocator", "service", "1.0",
                     allocator_ok,
                     "kmalloc returned NULL on first allocation");
    }

    kernel_events_init();
    boot_require("event queue", "service", "1.0",
                 event_queue_is_ready(),
                 "event queue failed to initialize");

    scheduler_init(pit_get_frequency_hz());
    nexus_watchdog_init(pit_get_frequency_hz());
    boot_require("scheduler", "service", "1.0",
                 scheduler_is_ready(),
                 "scheduler failed to initialize");
    console_component_status("watchdog", "service", "1.0", 1);

    {
        uint64_t service_tid = thread_create(scheduler_service_thread, NULL);
        boot_require("scheduler thread", "service", "1.0",
                     service_tid != 0,
                     "failed to create scheduler service thread");
    }

    for (int i = 0; i < 16; i++) pic_set_mask(i, i != 0 && i != 1 && i != 2 && i != 12); /* IRQ2 = cascade to the slave PIC, required for IRQ12 (mouse) */
    console_component_status("interrupt routing", "service", "1.0", 1);

    /* ── Драйверы устройств (через модульную систему) ─────────────────── */
    module_manager_init();
    (void)module_load_all();

    /* ── Диагностика USB и ввода — обязательна для загрузки ──────────── */
    {
        int kbd_ps2   = keyboard_is_present();
        int mouse_ps2 = mouse_is_present();
        int usb_ok    = (usb_host_type() != USB_HOST_NONE);
        int kbd_usb   = usb_keyboard_present();
        int mouse_usb = usb_mouse_present();

        /* PS/2: опционально — некоторые машины его не имеют. */
        console_component_status("PS/2 keyboard", "driver", keyboard_driver_version(), kbd_ps2);
        console_component_status("PS/2 mouse",    "driver", mouse_driver_version(), mouse_ps2);

        /* USB-хост: обязателен если нет PS/2. */
        console_component_status("USB host", "driver", NEXUS_VERSION_STRING, usb_ok);

        /* Всегда выводим полную диагностику xHCI */
        {
            int ctrl_count = xhci_controller_count();
            console_print("  [USB] xHCI controllers found: ");
            char num_buf[8];
            int n = ctrl_count;
            if (n == 0) { num_buf[0]='0'; num_buf[1]='\0'; }
            else {
                int idx=0; int tmp=n;
                while(tmp>0){num_buf[idx++]=(char)('0'+tmp%10);tmp/=10;}
                num_buf[idx]='\0';
                /* reverse */
                for(int a=0,b=idx-1;a<b;a++,b--){char t=num_buf[a];num_buf[a]=num_buf[b];num_buf[b]=t;}
            }
            console_print(num_buf);
            console_print("\n");

            console_print("  [USB] last xHCI error: ");
            console_print(xhci_last_error());
            console_print("\n");

            console_print("  [USB] last USB error:  ");
            console_print(usb_last_error());
            console_print("\n");

            if (usb_ok) {
                console_print("  [USB] ports: ");
                n = (int)xhci_port_count();
                if (n == 0) { num_buf[0]='0'; num_buf[1]='\0'; }
                else {
                    int idx=0; int tmp=n;
                    while(tmp>0){num_buf[idx++]=(char)('0'+tmp%10);tmp/=10;}
                    num_buf[idx]='\0';
                    for(int a=0,b=idx-1;a<b;a++,b--){char t=num_buf[a];num_buf[a]=num_buf[b];num_buf[b]=t;}
                }
                console_print(num_buf);
                console_print(" total, ");
                n = (int)xhci_connected_ports();
                if (n == 0) { num_buf[0]='0'; num_buf[1]='\0'; }
                else {
                    int idx=0; int tmp=n;
                    while(tmp>0){num_buf[idx++]=(char)('0'+tmp%10);tmp/=10;}
                    num_buf[idx]='\0';
                    for(int a=0,b=idx-1;a<b;a++,b--){char t=num_buf[a];num_buf[a]=num_buf[b];num_buf[b]=t;}
                }
                console_print(num_buf);
                console_print(" connected\n");
            }
        }

        if (usb_ok) {
            console_component_status("USB keyboard", "driver", NEXUS_VERSION_STRING, kbd_usb);
            console_component_status("USB mouse",    "driver", NEXUS_VERSION_STRING, mouse_usb);
        }

        /* Без клавиатуры (любого типа) ОС нельзя использовать — стоп. */
        if (!kbd_ps2 && !kbd_usb) {
            boot_require("keyboard (PS/2 or USB)", "driver", NEXUS_VERSION_STRING, 0,
                         "no keyboard detected; cannot continue boot");
        }

        /* Без мыши GUI не работает — стоп. */
        if (!mouse_ps2 && !mouse_usb) {
            boot_require("mouse (PS/2 or USB)", "driver", NEXUS_VERSION_STRING, 0,
                         "no mouse detected; cannot start graphical desktop");
        }
    }

    /* ── Система хранения: NVMe предпочтительнее, SATA/AHCI — fallback ── */
    if (storage_init()) {
        console_print("Storage backend: ");
        console_print(storage_backend_name());
        console_print("\n");
        if (fat32_mount(0)) {
            char disk_source[16];
            const char *disk = storage_device_name();
            int di = 0;
            while (disk[di] && di < 12) { disk_source[di] = disk[di]; ++di; }
            disk_source[di++] = 'p';
            disk_source[di++] = '0';
            disk_source[di] = '\0';
            vfs_mount(disk_source, "/mnt/disk0", "fat32", 0);
        }
    } else {
        console_print("Storage: no supported NVMe or SATA device found\n");
    }

    /* ── Процессы и системные сервисы ─────────────────────────────────── */
    /*
     * NexusOS boots directly into the graphical desktop. The command-line
     * shell is still accessible by pressing Escape from the desktop, and
     * 'desktop-run' re-enters the GUI at any time.
     */
    process_init();
    boot_require("process", "service", "1.0",
                 process_is_ready(),
                 "process subsystem failed to initialize");

    {
        int services_ok = nexus_session_init() && nexus_power_init() &&
                          nexus_system_info_init() && nexus_app_manager_init() &&
                          nexus_package_manager_init();
        if (services_ok) {
            nexus_app_register_builtin("nexus.files",    "Files",    "builtin:files");
            nexus_app_register_builtin("nexus.terminal", "Terminal", "builtin:terminal");
            nexus_app_register_builtin("nexus.settings", "Settings", "builtin:settings");
            (void)nexus_package_discover("/system/apps");
            (void)nexus_package_discover("/userdata/apps");
        }
        boot_require("system services", "service", "1.0",
                     services_ok,
                     "one or more system services failed to initialize");
    }

    /* ── Пользовательский режим и GUI ────────────────────────────────── */
    usermode_init();
    console_component_status("usermode", "service", "1.0", 1);

    syscall_init();
    console_component_status("syscall", "service", "1.0", 1);

    /* pci_scan() is intentionally NOT repeated here: the "pci" driver module
     * already ran it during module_load_all() (priority 100). Re-scanning now
     * would rebuild the PCI device table after the USB module has already
     * stored indices into it, invalidating those references and silently
     * breaking keyboard / mouse enumeration on every usb_poll() call.
     * gpu_init() is similarly omitted: the "gpu" module (priority 410) handles it. */

    gui_init(&boot_info->fb);
    console_component_status("gui",   "service", "1.0", 1);
    console_component_status("shell", "service", "1.0", 1);

    shell_init();

    __asm__ volatile ("sti");

    for (;;) {
        nexus_watchdog_heartbeat();
        if (!heap_validate()) {
            critical_os_stop("kernel heap integrity check failed");
        }
        if (nexus_watchdog_trip_pending()) {
            nexus_watchdog_clear_trip();
            critical_os_stop("normal kernel execution stalled for more than 3 seconds");
        }

        /* All keyboard/mouse/timer work captured by IRQ handlers is drained
         * here in normal kernel context, with interrupts enabled. This is the
         * event-queue boundary: shell commands are no longer
         * executed from an interrupt handler. */
        kernel_events_process();
        /* PS/2 safety net: pick up mouse bytes if an IRQ12 was ever lost. */
        (void)mouse_poll_idle();
        (void)keyboard_poll_idle();
        /* usb_poll() is already called inside kernel_events_process() on every
         * NEXUS_EVENT_TIMER_TICK. A second call here would advance g_evt_deq
         * a second time within the same tick, corrupting the xHCI event-ring
         * cycle bit and silently dropping keyboard/mouse transfer events. */
        if (!gui_is_active()) {
            shell_process_pending();
        }
        if (gui_is_active()) {
            console_cursor_disable();
            gui_update();
        } else {
            console_cursor_tick();
        }
        if (gui_wants_spin()) {
            __asm__ volatile ("pause");     /* frame due within ~2 ms: poll, do not sleep */
        } else {
            __asm__ volatile ("hlt");
        }
    }
}
