/* Минимальный отладочный shell ядра. Позже уедет в user space. */
#include <kernel/kernel.h>
#include <kernel/arch.h>
#include <miga/input.h>
#include <miga/time.h>
#include <miga/string.h>
#include <miga/io.h>
#include <kernel/sysinfo.h>
#include <miga/usb.h>
#include <miga/gpu.h>
#include <miga/disk.h>
#include <miga/net.h>
#include <miga/bt.h>
#include <kernel/display.h>
#include <kernel/acpi.h>
#include <miga/audio.h>
#include <kernel/gui.h>
#include <kernel/settings.h>

static void cmd_mem(void)
{
    kprintf("pages: total %lu, free %lu (%lu MiB free)\n", pmm_total_pages(), pmm_free_pages(),
            pmm_free_pages() * PAGE_SIZE >> 20);
}

static void cmd_cpu(void)
{
    sysinfo_cpu_print();
    struct cpu_live l;
    if (sysinfo_cpu_live(&l)) { kprintf("  live: timer not running, skipped\n"); return; }
    kprintf("  live (100 ms): TSC ~%u MHz", l.tsc_mhz);
    if (l.eff_mhz) kprintf(", effective ~%u MHz (APERF/MPERF)", l.eff_mhz);
    kprintf("\n");
    if (l.temp_c > -1000) kprintf("  temperature %d C (Tjmax %u)%s\n", l.temp_c, l.tjmax, l.throttled ? ", THROTTLING" : "");
    else kprintf("  temperature: n/a (needs Intel DTS on real hardware)\n");
}

static void cmd_uptime(void)
{
    uint64_t ms = time_uptime_ms();
    kprintf("up %lu.%03lu s (%lu ticks)\n", ms / 1000, ms % 1000, time_ticks());
}

static void cmd_usb(void)
{
    struct usb_ctrl_info c;
    if (usb_ctrl_get(&c)) { kprintf("USB: no xHCI controller\n"); return; }
    kprintf("xHCI %x.%02x at %#lx: %u ports, %u slots, %u device(s)\n", c.version >> 8, c.version & 0xFF,
            c.mmio, c.ports, c.slots, usb_dev_count());
    for (unsigned i = 0; i < usb_dev_count(); i++) {
        struct usb_dev_info d;
        if (usb_dev_get(i, &d)) continue;
        kprintf("port %u slot %u: %04x:%04x USB %x.%02x %s  %s%s\n", (unsigned)d.port, (unsigned)d.slot,
                (unsigned)d.vid, (unsigned)d.pid, d.bcd_usb >> 8, d.bcd_usb & 0xFF, usb_speed_name(d.speed),
                d.product[0] ? d.product : "(no name)", d.is_kbd ? "  [keyboard]" : "");
        kprintf("    class %02x (%s)", (unsigned)d.dev_class, usb_class_name(d.dev_class));
        for (unsigned k = 0; k < d.nifaces && k < USB_INFO_IFACES; k++)
            kprintf("  if%u=%02x/%02x/%02x(%s)", k, (unsigned)d.iface[k][0], (unsigned)d.iface[k][1],
                    (unsigned)d.iface[k][2], usb_class_name(d.iface[k][0]));
        kprintf("\n");
    }
}

static void cmd_gpu(void)
{
    if (!gpu_count()) { kprintf("GPU: no display adapters\n"); return; }
    for (unsigned i = 0; i < gpu_count(); i++) {
        struct gpu_info g;
        if (gpu_get(i, &g)) continue;
        kprintf("%02x:%02x.%u %s %04x:%04x rev %02x  %s (%s)%s\n", (unsigned)g.bus, (unsigned)g.dev,
                (unsigned)g.fn, g.vendor_name, (unsigned)g.vendor, (unsigned)g.device, (unsigned)g.rev,
                g.family, g.integrated ? "iGPU" : "dGPU", g.boot_fb ? "  [boot fb]" : "");
        if (g.link_width)
            kprintf("    PCIe %s GT/s x%u (max %s GT/s x%u)\n", gpu_link_gts(g.link_speed), (unsigned)g.link_width,
                    gpu_link_gts(g.link_max_speed), (unsigned)g.link_max_width);
        kprintf("    VRAM aperture %lu MiB", g.vram_aperture >> 20);
        if (g.chip_id) kprintf(", chip id %#x", (unsigned)g.chip_id);
        kprintf("\n");
    }
}

static void cmd_mouse(void)
{
    if (!mouse_count()) { kprintf("mouse: none found (PS/2 or USB HID boot mouse)\n"); return; }
    for (unsigned i = 0; i < mouse_count(); i++) kprintf("mouse %u: %s\n", i, mouse_kind(i));
}

static void cmd_hz(const char *arg)
{
    if (*arg) { unsigned v = 0; for (; *arg >= '0' && *arg <= '9'; arg++) v = v * 10 + (unsigned)(*arg - '0'); settings_set_int("gui.hz", (int)v); }
    kprintf("GUI frame rate target: %u Hz (EDID: %u Hz), TSC %u kHz\n", display_refresh_hz() ? display_refresh_hz() : 240,
            display_refresh_hz(), time_tsc_khz());
}

static void cmd_reboot(void)
{
    outb(0x64, 0xFE); /* пульс на reset-линию через контроллер клавиатуры */
    for (;;) cpu_hlt();
}

static void run(const char *l)
{
    if (!*l) return;
    if (!strcmp(l, "help"))        { kprintf("help mem ram cpu uptime usb lspci, lspci -v, board acpi time audio gpu disk net bt monitor mouse hz[ N] gui clear reboot halt crash\n"); kcmds_help(); }
    else if (!strcmp(l, "mem"))    cmd_mem();
    else if (!strcmp(l, "ram"))    sysinfo_ram_print();
    else if (!strcmp(l, "cpu"))    cmd_cpu();
    else if (!strcmp(l, "uptime")) cmd_uptime();
    else if (!strcmp(l, "usb"))    cmd_usb();
    else if (!strcmp(l, "gpu"))    cmd_gpu();
    else if (!strcmp(l, "mouse"))  cmd_mouse();
    else if ((l[0]==0x68 && l[1]==0x7a)  && (!l[2] || l[2] == 0x20)) cmd_hz(l[2] ? l + 3 : l + 2);
    else if (!strcmp(l, "disk"))   disk_print_all();
    else if (!strcmp(l, "net"))    net_print_all();
    else if (!strcmp(l, "bt"))     bt_print_all();
    else if (!strcmp(l, "monitor")) display_print();
    else if (!strcmp(l, "lspci"))  pci_log();
    else if (!strcmp(l, "lspci -v")) pci_log_ex(1);
    else if (!strcmp(l, "board"))  sysinfo_board_print();
    else if (!strcmp(l, "acpi"))   acpi_print();
    else if (!strcmp(l, "time"))   timeinfo_print();
    else if (!strcmp(l, "audio"))  audio_print_all();
    else if (!strcmp(l, "gui"))    { if (gui_run()) kprintf("gui: not available (no framebuffer or screen < 640x480)\n"); }
    else if (!strcmp(l, "clear"))  kprintf("\f");
    else if (!strcmp(l, "reboot")) cmd_reboot();
    else if (!strcmp(l, "halt"))   { kprintf("halted\n"); cpu_cli(); for (;;) cpu_hlt(); }
    else if (!strcmp(l, "crash"))  __asm__ volatile("ud2");
    else if (!kcmds_exec(l))       kprintf("unknown command: %s (try 'help')\n", l);
}

void kshell_exec(const char *line) { run(line); }

void kshell_run(void)
{
    char line[128];
    unsigned n = 0;
    kprintf("Type 'help'. Welcome to MigaOS!\nmiga> ");
    for (;;) {
        int c = input_getc();
        if (c < 0) { cpu_idle(); continue; }
        if (c == '\n') {
            kprintf("\n");
            line[n] = 0; n = 0;
            run(line);
            kprintf("miga> ");
        } else if (c == '\b') {
            if (n) { n--; kprintf("\b \b"); }
        } else if (c >= 32 && c < 127 && n < sizeof line - 1) {
            line[n++] = (char)c;
            kprintf("%c", c);
        }
    }
}
