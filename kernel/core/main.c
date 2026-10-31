#include <kernel/kernel.h>
#include <kernel/arch.h>
#include <kernel/sysinfo.h>
#include <kernel/display.h>
#include <kernel/acpi.h>
#include <kernel/gui.h>
#include <kernel/vfs.h>

void kmain(struct boot_info *bi)
{
    if (!bi || bi->magic != MIGA_BOOT_MAGIC)
        for (;;) cpu_hlt(); /* нас запустили не MigaBoot: сказать нечего и некому */

    drivers_init(bi, DRV_EARLY);          /* serial, fbcon: чтобы видеть логи */

    kprintf("\nMigaOS 0.1 (x86_64, UEFI boot)\n");
    kprintf("[boot] kernel %#lx..%#lx, framebuffer %ux%u @ %#lx, RSDP %#lx\n",
            bi->kernel_base, bi->kernel_base + bi->kernel_size,
            bi->fb.width, bi->fb.height, bi->fb.base, bi->rsdp);

    gdt_init();
    idt_init();
    pic_init();
    paging_init(bi);                      /* свои таблицы страниц до выдачи памяти PMM */
    time_init();                          /* TSC-часы для кадров 240 Гц */
    display_init(bi);                     /* EDID скопирован загрузчиком в MEM_LOADER; забираем до pmm_init */
    acpi_init(bi);                        /* таблицы ACPI тоже читаем до pmm_init */
    sysinfo_init(bi);                     /* CPUID/ACPI/SMBIOS: читаем ДО pmm_init, он затирает свободную память */
    pmm_init(bi);
    sysinfo_cpu_print();
    sysinfo_ram_print();
    display_print();
    sysinfo_board_print();
    acpi_print();
    timeinfo_print();

    pci_log();                            /* что есть на шине, до probe драйверов */
    drivers_init(bi, DRV_NORMAL);         /* pit, ps2kbd, xhci: нужны IRQ/PMM/MMIO */
    drivers_init(bi, DRV_LATE);           /* usbhid: поверх xhci */
    vfs_init();                           /* таблица точек монтирования: /, /boot, /mnt/diskN (файловых систем нет) */

    gui_init(bi);                         /* GUI-сервер: запоминает фреймбуфер, окно откроет команда `gui` */

    cpu_sti();
    gui_run();                            /* GUI стартует сам; Ctrl+Q выходит в kshell; без фреймбуфера вернётся сразу */
    kshell_run();
}
